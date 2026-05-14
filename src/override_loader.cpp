#define WIN32_LEAN_AND_MEAN
#include "hook.hpp"
#include "logs.hpp"
#include "override_loader.hpp"
#include "pattern_scanner.hpp"
#include "ue3_types.hpp"
#include "util.hpp"
#include <atomic>
#include <cstring>
#include <mutex>
#include <psapi.h>
#include <sstream>
#include <unordered_map>
#include <windows.h>

#ifdef _WIN64
static constexpr size_t kUObjOff_Outer = 0x40;
static constexpr size_t kUObjOff_Name = 0x48;
static constexpr size_t kUObjSz = 0x60;
#else
static constexpr size_t kUObjOff_Outer = 0x28;
static constexpr size_t kUObjOff_Name = 0x2c;
static constexpr size_t kUObjSz = 0x3c;
#endif

static constexpr int kSlot_Serialize = 2;
static constexpr int kSlot_IsLoading = 5;
static constexpr int kSlot_IsSaving = 6;
static constexpr int kSlot_Ver = 8;
static constexpr int kSlot_LicVer = 9;
static constexpr int kVtabSlots = 32;

static const char *kPat_Preload =
    "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 81 EC A0 00 00 00";

namespace
{
	using OverrideMap =
	    std::unordered_map<std::wstring, override_loader::OverrideRecord>;
	static OverrideMap g_overrides;

	using Fn_Preload = void(__cdecl *)(void * /*linker*/, void * /*obj*/);
	static Fn_Preload g_orig_Preload = nullptr;

	static std::atomic<ptrdiff_t> g_farchive_vptr_off{-1};

	static void *g_fake_vt[kVtabSlots];
	static std::once_flag g_fake_vt_flag;

	struct BinCtx
	{
		const uint8_t *data = nullptr;
		size_t size = 0;
		size_t pos = 0;
		int32_t base_file_offset = -1;
		int32_t ver = 684;
		int32_t licver = 0;
	};

	static thread_local BinCtx *tls_ctx = nullptr;

	static thread_local int s_preload_depth = 0;

	static bool safe_read(const void *p, size_t sz)
	{
		return p && !IsBadReadPtr(p, sz);
	}

	static std::wstring fname_str(int32_t idx)
	{
		auto *names = ue3().FNameNames;
		if (!names || idx < 0 || idx >= names->Num)
			return {};
		void *entry = names->Data[idx];
		if (!entry || !safe_read(entry, ue3().name_layout.str_off + 4))
			return {};
		if (ue3().name_layout.is_unicode(entry))
		{
			const wchar_t *s = ue3().name_layout.uni(entry);
			if (!safe_read(s, 2))
				return {};
			return s;
		}
		const char *s = ue3().name_layout.ansi(entry);
		if (!safe_read(s, 1))
			return {};
		return to_wide(std::string(s));
	}

	static std::wstring get_uobj_path(const void *obj)
	{
		if (!obj || !ue3().FNameNames)
			return {};

		std::vector<std::wstring> parts;

		const void *cur = obj;
		for (int d = 0; d < 8 && cur; d++)
		{
			if (!safe_read(cur, kUObjOff_Name + 8))
				break;

			const auto *b = static_cast<const uint8_t *>(cur);

			int32_t fn_idx;
			memcpy(&fn_idx, b + kUObjOff_Name, 4);

			std::wstring seg = fname_str(fn_idx);
			if (seg.empty())
				break;
			parts.push_back(seg);

			void *outer = nullptr;
			memcpy(&outer, b + kUObjOff_Outer, sizeof(void *));
			cur = outer;
		}

		if (parts.empty())
			return {};

		std::wstring path;
		for (auto it = parts.rbegin(); it != parts.rend(); ++it)
		{
			if (!path.empty())
				path += L'.';
			path += *it;
		}
		return path;
	}

	static ptrdiff_t discover_farchive_vptr_off(const void *linker)
	{
		HMODULE mod = GetModuleHandleW(nullptr);
		MODULEINFO mi = {};
		GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi));
		const uintptr_t img_base = (uintptr_t)mi.lpBaseOfDll;
		const uintptr_t img_end = img_base + mi.SizeOfImage;

		const uint8_t *base = static_cast<const uint8_t *>(linker);
		void *primary_vptr;
		memcpy(&primary_vptr, base, sizeof(void *));

		for (size_t off = kUObjSz; off + sizeof(void *) <= /*0xC00*/ 0x2000; off += 8)
		{
			if (!safe_read(base + off, sizeof(void *)))
				continue;

			void *cand;
			memcpy(&cand, base + off, sizeof(void *));
			if (!cand || cand == primary_vptr)
				continue;

			uintptr_t cv = (uintptr_t)cand;
			if (cv < img_base || cv >= img_end)
				continue;

			if (!safe_read(cand, kVtabSlots * sizeof(void *)))
				continue;

			auto **vt = static_cast<void **>(cand);
			int valid = 0;
			for (int i = 0; i < kVtabSlots; ++i)
			{
				uintptr_t fn = (uintptr_t)vt[i];
				if (fn >= img_base && fn < img_end)
					++valid;
				else
					break;
			}
			if (valid < 8)
				continue;

			if (!vt[kSlot_Serialize])
				continue;

			log_info("override: FArchive subobj vptr at linker+0x%zx (vt=%p)",
			         off, cand);
			return (ptrdiff_t)off;
		}

		log_err("override: could not locate FArchive secondary vptr — "
		        "bin overrides disabled");
		return -1;
	}

	static void __cdecl override_Serialize(void * /*self*/, void *dst,
	                                       int32_t len)
	{
		BinCtx *ctx = tls_ctx;
		if (!ctx || len <= 0)
			return;
		const size_t want = (size_t)len;
		const size_t avail =
		    (ctx->pos < ctx->size) ? (ctx->size - ctx->pos) : 0;
		const size_t actual = (want <= avail) ? want : avail;
		if (actual > 0)
		{
			memcpy(dst, ctx->data + ctx->pos, actual);
			ctx->pos += actual;
		}
		if (actual < want)
			log_warn("override: Serialize overread — want=%zu avail=%zu", want,
			         avail);
	}

	static int32_t __cdecl override_IsLoading(void * /*self*/) { return 1; }

	static int32_t __cdecl override_IsSaving(void * /*self*/) { return 0; }

	static int32_t __cdecl override_Ver(void * /*self*/)
	{
		return tls_ctx ? tls_ctx->ver : 684;
	}

	static int32_t __cdecl override_LicVer(void * /*self*/)
	{
		return tls_ctx ? tls_ctx->licver : 0;
	}

	static void build_fake_vt(void **real_vt)
	{
		memcpy(g_fake_vt, real_vt, kVtabSlots * sizeof(void *));
		g_fake_vt[kSlot_Serialize] = (void *)&override_Serialize;
		g_fake_vt[kSlot_IsLoading] = (void *)&override_IsLoading;
		g_fake_vt[kSlot_IsSaving] = (void *)&override_IsSaving;
		g_fake_vt[kSlot_Ver] = (void *)&override_Ver;
		g_fake_vt[kSlot_LicVer] = (void *)&override_LicVer;
		log_info("override: fake vtable built at %p", (void *)g_fake_vt);
	}

	static void resolve_imports(override_loader::OverrideRecord &rec)
	{
		if (rec.imports_resolved)
			return;
		rec.imports_resolved = true;

		if (rec.imp_entries.empty())
			return;

		if (!ue3().StaticFindObjectFast)
		{
			log_warn(
			    "override: resolve_imports: StaticFindObjectFast not resolved");
			return;
		}

		using SFOFFn = void *(__cdecl *)(void *, void *, FNameOnStack, int32_t,
		                                 int32_t, uint64_t);
		auto sfof = reinterpret_cast<SFOFFn>(ue3().StaticFindObjectFast);

		for (auto &e : rec.imp_entries)
		{
			std::wstring wpath = to_wide(e.path);
			const wchar_t *dot = wcsrchr(wpath.c_str(), L'.');
			const wchar_t *obj_name = dot ? (dot + 1) : wpath.c_str();

			FNameOnStack fn{};
			if (ue3().FNameInit)
			{
				using FNameInitFn =
				    void(__cdecl *)(void *, const wchar_t *, int32_t);
				reinterpret_cast<FNameInitFn>(ue3().FNameInit)(&fn, obj_name,
				                                               1);
			}

			void *found = sfof(nullptr, nullptr, fn, 0, 1, 0);
			if (found)
			{
				rec.resolved_imports.push_back({e.mod_index, found});
				log_info("override: import [%d] '%s' -> %p", e.mod_index,
				         e.path.c_str(), found);
			}
			else
			{
				log_warn("override: import [%d] '%s' NOT FOUND", e.mod_index,
				         e.path.c_str());
				rec.resolved_imports.push_back({e.mod_index, nullptr});
			}
		}
	}

	static void __cdecl hooked_Preload(void *linker, void *obj)
	{
		if (s_preload_depth > 0)
		{
			g_orig_Preload(linker, obj);
			return;
		}

		if (!linker || !obj || !ue3().FNameNames)
		{
			g_orig_Preload(linker, obj);
			return;
		}

		std::wstring path = get_uobj_path(obj);
		if (path.empty())
		{
			g_orig_Preload(linker, obj);
			return;
		}

		auto *rec = override_loader::find(path);
		if (!rec || rec->bin.empty())
		{
			g_orig_Preload(linker, obj);
			return;
		}

		log_info("override: Preload hit '%ls'  bin=%zu bytes", path.c_str(),
		         rec->bin.size());

		resolve_imports(*rec);

		ptrdiff_t fa_off = g_farchive_vptr_off.load(std::memory_order_acquire);
		if (fa_off == -1)
		{
			fa_off = discover_farchive_vptr_off(linker);
			g_farchive_vptr_off.store(fa_off, std::memory_order_release);
		}

		if (fa_off < 0)
		{
			log_warn("override: FArchive offset unknown, passing through '%ls'",
			         path.c_str());
			g_orig_Preload(linker, obj);
			return;
		}

		uint8_t *linker_bytes = static_cast<uint8_t *>(linker);
		void **fa_vptr_loc = reinterpret_cast<void **>(linker_bytes + fa_off);
		void **real_fa_vt = static_cast<void **>(*fa_vptr_loc);

		std::call_once(g_fake_vt_flag, [&]() { build_fake_vt(real_fa_vt); });

		BinCtx ctx;
		ctx.data = rec->bin.data();
		ctx.size = rec->bin.size();
		ctx.pos = 0;
		ctx.ver = rec->upk_version;
		ctx.licver = 0;
		tls_ctx = &ctx;

		void *saved_fa_vptr = *fa_vptr_loc;
		*fa_vptr_loc = static_cast<void *>(g_fake_vt);

		++s_preload_depth;
		g_orig_Preload(linker, obj);
		--s_preload_depth;

		*fa_vptr_loc = saved_fa_vptr;
		tls_ctx = nullptr;

		log_info("override: Preload done '%ls'  consumed=%zu/%zu bytes",
		         path.c_str(), ctx.pos, ctx.size);
	}

	static std::vector<uint8_t> read_file(const std::wstring &path)
	{
		HANDLE h =
		    CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
		                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return {};

		LARGE_INTEGER sz{};
		GetFileSizeEx(h, &sz);
		if (sz.QuadPart == 0 || sz.QuadPart > 64 * 1024 * 1024)
		{
			CloseHandle(h);
			return {};
		}

		std::vector<uint8_t> buf((size_t)sz.QuadPart);
		DWORD read = 0;
		ReadFile(h, buf.data(), (DWORD)buf.size(), &read, nullptr);
		CloseHandle(h);
		if (read != buf.size())
			return {};
		return buf;
	}

	static std::wstring stem_from_filename(const wchar_t *fname)
	{
		std::wstring s(fname);
		auto dot = s.rfind(L'.');
		if (dot == std::wstring::npos)
			return {};
		return s.substr(0, dot);
	}

	static std::vector<override_loader::ImpEntry>
	parse_imp(const std::wstring &path)
	{
		auto raw = read_file(path);
		if (raw.empty())
			return {};

		std::vector<override_loader::ImpEntry> out;
		std::string text(raw.begin(), raw.end());
		std::istringstream ss(text);
		std::string line;

		bool in_imports = false;
		while (std::getline(ss, line))
		{
			auto hash = line.find('#');
			if (hash != std::string::npos)
				line = line.substr(0, hash);

			auto trim_s = [](std::string &s)
			{
				size_t a = s.find_first_not_of(" \t\r\n");
				size_t b = s.find_last_not_of(" \t\r\n");
				s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
			};
			trim_s(line);
			if (line.empty())
				continue;

			if (line == "[imports]")
			{
				in_imports = true;
				continue;
			}
			if (line.front() == '[')
			{
				in_imports = false;
				continue;
			}

			if (!in_imports)
				continue;

			auto eq = line.find('=');
			if (eq == std::string::npos)
				continue;
			std::string idx_s = line.substr(0, eq);
			std::string path_s = line.substr(eq + 1);
			trim_s(idx_s);
			trim_s(path_s);

			try
			{
				override_loader::ImpEntry e;
				e.mod_index = std::stoi(idx_s);
				e.path = path_s;
				out.push_back(e);
			}
			catch (...)
			{
				log_warn("override: bad .imp line: '%s'", line.c_str());
			}
		}
		return out;
	}

	static void scan_mod_overrides(const LoadedMod &lm)
	{
		std::wstring ov_dir = lm.dir_w + L"\\Overrides";
		if (GetFileAttributesW(ov_dir.c_str()) == INVALID_FILE_ATTRIBUTES)
			return;

		WIN32_FIND_DATAW fd{};
		HANDLE h = FindFirstFileW((ov_dir + L"\\*.bin").c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE)
			return;

		do
		{
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;

			std::wstring key = stem_from_filename(fd.cFileName);
			if (key.empty())
			{
				log_warn("override: skipping badly-named file '%ls'",
				         fd.cFileName);
				continue;
			}

			std::wstring bin_path = ov_dir + L"\\" + fd.cFileName;
			auto bin = read_file(bin_path);
			if (bin.empty())
			{
				log_warn("override: failed to read '%ls'", fd.cFileName);
				continue;
			}

			std::wstring imp_path = ov_dir + L"\\" + key + L".imp";
			std::vector<override_loader::ImpEntry> imps;
			if (GetFileAttributesW(imp_path.c_str()) != INVALID_FILE_ATTRIBUTES)
			{
				imps = parse_imp(imp_path);
				if (!imps.empty())
					log_info("override: '%ls': %zu import entries", key.c_str(),
					         imps.size());
			}

			override_loader::OverrideRecord rec;
			rec.key = key;
			rec.bin = std::move(bin);
			rec.imp_entries = std::move(imps);
			rec.upk_version = (int32_t)lm.cfg.upk_version;
			if (rec.upk_version == 0)
				rec.upk_version = 684;

			g_overrides[key] = std::move(rec);
			log_info("override: registered '%ls'  bin=%zu bytes  mod='%s'",
			         key.c_str(), g_overrides[key].bin.size(),
			         lm.cfg.name.c_str());

		} while (FindNextFileW(h, &fd));

		FindClose(h);
	}

}  // namespace

namespace override_loader
{
	void discover(const std::vector<LoadedMod> &mods)
	{
		for (const auto &lm : mods)
			scan_mod_overrides(lm);

		log_info("override_loader: discovery done — %zu override(s)",
		         g_overrides.size());
	}

	void install_hooks()
	{
		if (g_overrides.empty())
		{
			log_info("override_loader: no overrides registered — Preload hook "
			         "skipped");
			return;
		}

		void *preload_addr =
		    FindPatternString(GetModuleHandleW(nullptr), kPat_Preload);
		if (!preload_addr)
		{
			log_err("override_loader: ULinkerLoad::Preload NOT FOUND — bin "
			        "overrides "
			        "disabled");
			return;
		}

		hook::add(preload_addr, reinterpret_cast<void *>(&hooked_Preload),
		          reinterpret_cast<void **>(&g_orig_Preload));

		log_info("override_loader: Preload hook queued at %p  "
		         "(%zu override(s))",
		         preload_addr, g_overrides.size());
	}

	OverrideRecord *find(const std::wstring &key)
	{
		auto it = g_overrides.find(key);
		return (it != g_overrides.end()) ? &it->second : nullptr;
	}

	size_t count() { return g_overrides.size(); }

}  // namespace override_loader
