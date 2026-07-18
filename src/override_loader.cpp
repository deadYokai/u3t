#define WIN32_LEAN_AND_MEAN
#include "override_loader.hpp"
#include "hook.hpp"
#include "logs.hpp"
#include "ue3_api.hpp"
#include "ue3_layout.hpp"
#include "ue3_patch.hpp"
#include "util.hpp"
#include <cstring>
#include <memory>
#include <psapi.h>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <windows.h>

static constexpr int kVT_MAX = 64;

static inline int uobj_serialize_slot()
{
	return ue3().vt_Serialize
	           ? static_cast<int>(ue3().vt_Serialize / sizeof(void *))
	           : -1;
}

static inline int32_t fname_count()
{
	return ue3().FNameNamesArr ? static_cast<int32_t>(ue3raw::rd_u32(
	                                 ue3().FNameNamesArr, sizeof(void *)))
	                           : 0;
}

static void namemap_get(void *l, void *&d, int32_t &n, int32_t &m)
{
	uint8_t *p = static_cast<uint8_t *>(l) + ue3().l_NameMap;
	d = ue3raw::rd_ptr(p, 0);
	n = static_cast<int32_t>(ue3raw::rd_u32(p, sizeof(void *)));
	m = static_cast<int32_t>(ue3raw::rd_u32(p, sizeof(void *) + 4));
}

static void namemap_set(void *l, void *d, int32_t n, int32_t m)
{
	uint8_t *p = static_cast<uint8_t *>(l) + ue3().l_NameMap;
	memcpy(p, &d, sizeof(void *));
	memcpy(p + sizeof(void *), &n, 4);
	memcpy(p + sizeof(void *) + 4, &m, 4);
}

static void copy_live_version(FArchiveFields *dst, void *linker)
{
	const auto *src = reinterpret_cast<const FArchiveFields *>(
	    static_cast<const uint8_t *>(linker_farchive(linker)) + sizeof(void *));
	dst->ArVer = src->ArVer;
	dst->ArNetVer = src->ArNetVer;
	dst->ArLicenseeVer = src->ArLicenseeVer;
	dst->ArIsLoading = 1;
}

struct BufReader
{
	void *vt;
	FArchiveFields ar;
	const uint8_t *data;
	size_t size;
	size_t pos;
	int32_t serial_off;
	bool anchored;
};

static void *g_buf_vt[kVT_MAX];
static bool g_buf_vt_ready = false;

static void __fastcall br_Serialize(BufReader *self, UE3_EDX void *dst,
                                    int32_t len)
{
	if (len <= 0)
		return;
	const size_t want = static_cast<size_t>(len);
	const size_t avail = self->pos < self->size ? self->size - self->pos : 0;
	const size_t n = want <= avail ? want : avail;
	if (n)
		memcpy(dst, self->data + self->pos, n);
	self->pos += n;
	if (n < want)
		log_warn("override: br_Serialize overread want=%zu avail=%zu", want,
		         avail);
}

static int32_t __fastcall br_Tell(BufReader *self)
{
	return self->anchored ? self->serial_off + static_cast<int32_t>(self->pos)
	                      : static_cast<int32_t>(self->pos);
}

static void __fastcall br_Seek(BufReader *self, UE3_EDX int32_t pos)
{
	if (!self->anchored)
	{
		self->serial_off = pos;
		self->anchored = true;
		self->pos = 0;
	}
	else
	{
		const int32_t rel = pos - self->serial_off;
		if (rel >= 0 && static_cast<size_t>(rel) <= self->size)
			self->pos = static_cast<size_t>(rel);
	}
}

static int32_t __fastcall br_TotalSize(BufReader *self)
{
	return static_cast<int32_t>(self->size);
}

static int32_t __fastcall br_Precache(BufReader *, UE3_EDX int32_t, int32_t)
{
	return 1;
}

static int32_t __fastcall br_IsError(BufReader *) { return 0; }

static void build_buf_vt(void **real_vt)
{
	const FArchiveSlots &S = ue3().ar;
	const int n = S.total > 0 && S.total <= kVT_MAX ? S.total : kVT_MAX;
	memcpy(g_buf_vt, real_vt, n * sizeof(void *));
	if (S.Serialize >= 0)
		g_buf_vt[S.Serialize] = reinterpret_cast<void *>(&br_Serialize);
	if (S.Tell >= 0)
		g_buf_vt[S.Tell] = reinterpret_cast<void *>(&br_Tell);
	if (S.TotalSize >= 0)
		g_buf_vt[S.TotalSize] = reinterpret_cast<void *>(&br_TotalSize);
	if (S.Seek >= 0)
		g_buf_vt[S.Seek] = reinterpret_cast<void *>(&br_Seek);
	if (S.Precache >= 0)
		g_buf_vt[S.Precache] = reinterpret_cast<void *>(&br_Precache);
	if (S.GetError >= 0)
		g_buf_vt[S.GetError] = reinterpret_cast<void *>(&br_IsError);
	g_buf_vt_ready = true;
	log_info("override: BufReader vtable built from real_vt=%p",
	         (void *)real_vt);
}

struct FNamePatchCtx
{
	const int32_t *remap;
	size_t remap_size;
	void *orig_fn;
	void **orig_vt;
	void **patched_vt;
	void *active_br;
	void *fa;
};

static thread_local std::vector<FNamePatchCtx> tl_fn_stack;

static void *__fastcall fname_remap_thunk(void *fa, UE3_EDX void *fname_out)
{
	if (tl_fn_stack.empty())
	{
		log_warn("override: fname_remap_thunk called with no active patch");
		return fa;
	}
	FNamePatchCtx &ctx = tl_fn_stack.back();

	{
		void *lb = static_cast<uint8_t *>(fa) - ue3().l_FArchiveOff;
		void *cur = *linker_loader_ptr(lb);
		if (cur != ctx.active_br)
		{
			using FnT = void *(UE3_THISCALL *)(void *, void *);
			return reinterpret_cast<FnT>(ctx.orig_fn)(fa, fname_out);
		}
	}

	int32_t raw[2] = {};
	auto *br = static_cast<BufReader *>(ctx.active_br);
	if (br && br->pos + 8 <= br->size)
	{
		memcpy(raw, br->data + br->pos, 8);
		br->pos += 8;
	}
	else
	{
		log_warn("override: fname overread pos=%zu size=%zu", br ? br->pos : 0,
		         br ? br->size : 0);
	}

	int32_t name_index = 0;  // NAME_None fallback
	if (raw[0] >= 0 && ctx.remap &&
	    static_cast<size_t>(raw[0]) < ctx.remap_size)
	{
		name_index = ctx.remap[raw[0]];
	}
	else
	{
		log_warn("override: fname_remap: tool_idx=%d out of range "
		         "(remap_size=%zu) -> None",
		         raw[0], ctx.remap_size);
	}

	int32_t *out = static_cast<int32_t *>(fname_out);
	out[0] = name_index;
	out[1] = raw[1];
	return fa;
}

static void install_fname_patch(void *linker,
                                const override_loader::OverrideRecord &rec,
                                void *active_br)
{
	uint8_t *fa = static_cast<uint8_t *>(linker) + ue3().l_FArchiveOff;
	void **orig = *reinterpret_cast<void ***>(fa);

	const int fslot = ue3().ar.SerializeName;
	const int n = ue3().ar.total > 0 ? ue3().ar.total : kVT_MAX;
	void **pv = new void *[n];
	memcpy(pv, orig, n * sizeof(void *));
	if (fslot >= 0)
		pv[fslot] = reinterpret_cast<void *>(&fname_remap_thunk);

	tl_fn_stack.push_back({rec.name_remap.data(), rec.name_remap.size(),
	                       fslot >= 0 ? orig[fslot] : nullptr, orig, pv,
	                       active_br, fa});

	*reinterpret_cast<void ***>(fa) = pv;
	log_info("override: fname patch fa=%p orig_vt=%p pv=%p depth=%zu",
	         (void *)fa, (void *)orig, (void *)pv, tl_fn_stack.size());
}

static void remove_fname_patch(void *linker)
{
	uint8_t *fa = static_cast<uint8_t *>(linker) + ue3().l_FArchiveOff;
	if (tl_fn_stack.empty() || tl_fn_stack.back().fa != fa)
	{
		log_warn("override: remove_fname_patch mismatch fa=%p (stack top=%p) — "
		         "skipping restore to avoid corrupting an unrelated patch",
		         (void *)fa,
		         tl_fn_stack.empty() ? nullptr : tl_fn_stack.back().fa);
		return;
	}
	const FNamePatchCtx ctx = tl_fn_stack.back();
	tl_fn_stack.pop_back();
	*reinterpret_cast<void ***>(fa) = ctx.orig_vt;
	delete[] ctx.patched_vt;
	log_info("override: fname patch removed fa=%p depth=%zu", (void *)fa,
	         tl_fn_stack.size());
}

static bool safe_read(const void *p, size_t n)
{
	return p && !IsBadReadPtr(p, n);
}

static std::wstring fname_str(int32_t gidx)
{
	void *entry = fname_entry(gidx);
	if (!entry || !safe_read(entry, ue3().name.str_off + 4))
		return {};
	if (ue3().name.is_unicode(entry))
	{
		const wchar_t *s = ue3().name.uni(entry);
		return safe_read(s, 2) ? std::wstring(s) : std::wstring{};
	}
	const char *s = ue3().name.ansi(entry);
	return safe_read(s, 1) ? to_wide(std::string(s)) : std::wstring{};
}

static void debug_dump_chain(const void *obj)
{
	const void *cur = obj;
	for (int d = 0; d < 8 && cur; ++d)
	{
		if (!safe_read(cur, ue3().sizeof_UObject))
		{
			log_info("  [%d] ptr=%p UNREADABLE", d, cur);
			break;
		}
		const int32_t idx = uobj_name_index(cur);
		const int32_t num = uobj_name_number(cur);
		const void *outer = uobj_outer(cur);
		std::wstring s = fname_str(idx);
		log_info(
		    "  [%d] ptr=%p outer=%p name.Index=%d name.Number=%d str='%ls'", d,
		    cur, outer, idx, num, s.empty() ? L"<oob>" : s.c_str());
		cur = outer;
	}
}

static constexpr int kMaxOuterDepth = 16;

static std::wstring get_uobj_path(const void *obj)
{
	if (!obj || !ue3().FNameNamesArr || !safe_read(obj, ue3().sizeof_UObject))
		return {};

	std::vector<std::wstring> parts;
	const void *cur = obj;
	for (int d = 0; d < kMaxOuterDepth && cur; ++d)
	{
		if (!safe_read(cur, ue3().sizeof_UObject))
			return {};
		const int32_t idx = uobj_name_index(cur);
		const int32_t num = uobj_name_number(cur);
		std::wstring seg = fname_str(idx);
		if (seg.empty())
			return {};
		if (num != 0)
			seg += L"_" + std::to_wstring(num - 1);
		parts.push_back(seg);
		cur = uobj_outer(cur);
	}
	if (parts.empty() || cur)
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

static void build_name_remap(override_loader::OverrideRecord &rec, void *linker)
{
	rec.name_remap_ready = true;
	if (rec.tool_names.empty())
		return;

	TArrayView nm = linker_namemap(linker);
	log_info("override: build_name_remap '%ls' nm=%p Num=%d", rec.key.c_str(),
	         (void *)nm.data, nm.num);

	using FNameInitFn = void(UE3_THISCALL *)(void *self, const wchar_t *InName,
	                                         int32_t InNumber, int32_t FindType,
	                                         int32_t bSplitName);

	auto fni = reinterpret_cast<FNameInitFn>(ue3().FNameInit);
	if (!fni)
	{
		log_warn("override: build_name_remap '%ls' — FNameInit unresolved",
		         rec.key.c_str());
		rec.name_remap.clear();
		return;
	}

	rec.name_remap.assign(rec.tool_names.size(), 0);
	for (size_t ti = 0; ti < rec.tool_names.size(); ++ti)
	{
		const std::wstring w = to_wide(rec.tool_names[ti]);
		FNameStack tmp{};
		fni(&tmp, w.c_str(), 0, 1, 0);
		rec.name_remap[ti] = tmp.Index;
	}

	if (!rec.name_remap.empty())
	{
		auto *name_map = reinterpret_cast<UE3TArray *>(
		    static_cast<uint8_t *>(linker) + ue3().l_NameMap);
		UE3FName *existing = reinterpret_cast<UE3FName *>(name_map->Data);
		int existing_num = name_map->Num;

		std::vector<UE3FName> to_add;
		rec.name_map_final.resize(rec.name_remap.size());

		for (size_t i = 0; i < rec.name_remap.size(); ++i)
		{
			UE3FName fname{rec.name_remap[i], 0};
			bool found = false;
			for (int j = 0; j < existing_num; ++j)
			{
				if (memcmp(&existing[j], &fname, sizeof(UE3FName)) == 0)
				{
					rec.name_map_final[i] = j;
					found = true;
					break;
				}
			}
			if (!found)
			{
				rec.name_map_final[i] =
				    existing_num + static_cast<int>(to_add.size());
				to_add.push_back(fname);
			}
		}

		if (!to_add.empty())
		{
			int base = ue3_append_names(linker, to_add.data(),
			                            static_cast<int>(to_add.size()));
			(void)base;
		}
		log_info("override: processed %zu names, %zu new appended",
		         rec.name_remap.size(), to_add.size());
	}

	log_info("override: name_remap '%ls' %zu/%zu interned", rec.key.c_str(),
	         rec.tool_names.size(), rec.tool_names.size());
}

using PreloadFn = void(UE3_THISCALL *)(void *, void *);
static PreloadFn g_orig_Preload = nullptr;

static void call_object_serialize(void *obj, void *linker)
{
	void *farchive = static_cast<uint8_t *>(linker) + ue3().l_FArchiveOff;
	const int slot = uobj_serialize_slot();
	if (slot < 0)
	{
		log_warn("override: Serialize slot unknown — should not reach here");
		return;
	}

	void *prev_serial = nullptr;
	if (ue3().GSerializedObject)
	{
		prev_serial = *ue3().GSerializedObject;
		*ue3().GSerializedObject = obj;
	}

	using SerializeFn = void(UE3_THISCALL *)(void *, void *);
	void **vt = *static_cast<void ***>(obj);
	auto serialize = reinterpret_cast<SerializeFn>(vt[slot]);
	serialize(obj, farchive);

	if (ue3().GSerializedObject)
		*ue3().GSerializedObject = prev_serial;
}

static void do_override_preload(void *linker, void *obj, void *exp,
                                override_loader::OverrideRecord &rec)
{
	const bool vtslot_known = uobj_serialize_slot() >= 0;

	if (uobj_has_flag(obj, RF_ClassDefaultObject) || !vtslot_known)
	{
		g_orig_Preload(linker, obj);
		return;
	}

	void *real_loader = *linker_loader_ptr(linker);
	void *real_original = *linker_original_loader_ptr(linker);

	if (!real_loader || !safe_read(real_loader, sizeof(void *)))
	{
		log_warn("override: Loader invalid for '%ls', skipping",
		         rec.key.c_str());
		return;
	}

	void **rvt = *static_cast<void ***>(real_loader);
	if (!g_buf_vt_ready)
		build_buf_vt(rvt);
	if (!rec.name_remap_ready)
		build_name_remap(rec, linker);

	const bool has_remap = !rec.name_remap.empty();
	BufReader br{};
	br.vt = g_buf_vt;
	copy_live_version(&br.ar, linker);
	br.data = rec.bin.data();
	br.size = rec.bin.size();
	void *br_ptr = &br;

	*linker_loader_ptr(linker) = br_ptr;
	*linker_original_loader_ptr(linker) = br_ptr;

	if (has_remap)
	{
		install_fname_patch(linker, rec, br_ptr);
	}

	void *saved_nm_data = nullptr;
	int32_t saved_nm_num = 0, saved_nm_max = 0;
	namemap_get(linker, saved_nm_data, saved_nm_num, saved_nm_max);
	std::vector<FNameStack> shadow_nm;
	if (has_remap)
	{
		shadow_nm.resize(rec.name_remap.size());
		for (size_t i = 0; i < rec.name_remap.size(); ++i)
			shadow_nm[i] = FNameStack{rec.name_remap[i], 0};
		namemap_set(linker, shadow_nm.data(),
		            static_cast<int32_t>(shadow_nm.size()),
		            static_cast<int32_t>(shadow_nm.size()));
	}

#if _WIN64
	br_Seek(&br, exp_serial_offset(exp));
#else
	br_Seek(&br, nullptr, exp_serial_offset(exp));
#endif

	uobj_clear_flag(obj, RF_NeedLoad);

	call_object_serialize(obj, linker);

	if (has_remap)
	{
		namemap_set(linker, saved_nm_data, saved_nm_num, saved_nm_max);
		remove_fname_patch(linker);
	}

	*linker_loader_ptr(linker) = real_loader;
	*linker_original_loader_ptr(linker) = real_original;

	log_info("override: full-port '%ls' consumed=%zu/%zu bytes",
	         rec.key.c_str(), br.pos, br.size);
}

static inline void orig_preload(void *linker, void *obj)
{
	if (linker && g_orig_Preload)
		g_orig_Preload(linker_farchive(linker), obj);
}

static void __fastcall hooked_Preload(void *farchive, UE3_EDX void *obj)
{
	void *linker = farchive
	                   ? static_cast<uint8_t *>(farchive) - ue3().l_FArchiveOff
	                   : nullptr;

	if (!ue3().ok || !linker || !obj || !ue3().FNameNamesArr)
	{
		orig_preload(linker, obj);
		return;
	}

	if (!safe_read(obj, ue3().sizeof_UObject))
	{
		orig_preload(linker, obj);
		return;
	}

	if (!uobj_has_flag(obj, RF_NeedLoad))
	{
		orig_preload(linker, obj);
		return;
	}

	if (uobj_linker(obj) != linker)
	{
		orig_preload(linker, obj);
		return;
	}

	std::wstring path = get_uobj_path(obj);
	if (path.empty())
	{
		orig_preload(linker, obj);
		return;
	}

	auto *rec = override_loader::find(path);
	if (!rec || rec->bin.empty())
	{
		orig_preload(linker, obj);
		return;
	}

	log_info("override: Preload '%ls' (%zu bytes)", path.c_str(),
	         rec->bin.size());

	if (!uobj_has_flag(obj, RF_NeedLoad))
	{
		log_info("override: '%ls' RF_NeedLoad already clear after re-check",
		         path.c_str());
		return;
	}

	const int32_t idx = uobj_linker_index(obj);
	void *exp = lk_export(linker, idx);
	if (!exp)
	{
		log_warn("override: '%ls' — bad _LinkerIndex %d, passing through",
		         path.c_str(), idx);
		orig_preload(linker, obj);
		return;
	}

	if (exp_export_flags(exp) & EF_ScriptPatcherExport)
	{
		log_warn("override: '%ls' has EF_ScriptPatcherExport — passing through",
		         path.c_str());
		orig_preload(linker, obj);
		return;
	}

	do_override_preload(linker, obj, exp, *rec);
}

static std::unordered_map<std::wstring, override_loader::OverrideRecord>
    g_overrides;

static std::vector<uint8_t> read_file(const std::wstring &path)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
	                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return {};
	LARGE_INTEGER sz{};
	GetFileSizeEx(h, &sz);
	if (sz.QuadPart <= 0 || sz.QuadPart > 64 * 1024 * 1024)
	{
		CloseHandle(h);
		return {};
	}
	std::vector<uint8_t> buf(static_cast<size_t>(sz.QuadPart));
	DWORD rd = 0;
	ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &rd, nullptr);
	CloseHandle(h);
	return rd == buf.size() ? buf : std::vector<uint8_t>{};
}

static std::vector<std::string> parse_namemap(const std::wstring &path)
{
	auto raw = read_file(path);
	if (raw.empty())
		return {};
	std::vector<std::string> out;
	std::string text(raw.begin(), raw.end());
	std::istringstream ss(text);
	std::string line;
	while (std::getline(ss, line))
	{
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (!line.empty())
			out.push_back(line);
	}
	return out;
}

static std::wstring file_stem(const wchar_t *fname)
{
	std::wstring s(fname);
	auto dot = s.rfind(L'.');
	return dot == std::wstring::npos ? std::wstring{} : s.substr(0, dot);
}

static void scan_package_dir(const std::wstring &pkg_dir,
                             const std::wstring &pkg_name, const LoadedMod &lm)
{
	std::shared_ptr<std::vector<std::string>> names;
	{
		std::wstring nm = pkg_dir + L"\\" + pkg_name + L".namemap";
		if (GetFileAttributesW(nm.c_str()) != INVALID_FILE_ATTRIBUTES)
		{
			auto v = parse_namemap(nm);
			if (!v.empty())
				names =
				    std::make_shared<std::vector<std::string>>(std::move(v));
		}
	}
	if (!names)
		log_warn("override: no .namemap in '%ls' — FName remap disabled",
		         pkg_dir.c_str());

	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((pkg_dir + L"\\*.bin").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;
	do
	{
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		std::wstring stem = file_stem(fd.cFileName);
		if (stem.empty())
			continue;

		auto bin = read_file(pkg_dir + L"\\" + fd.cFileName);
		if (bin.empty())
		{
			log_warn("override: read failed '%ls'", fd.cFileName);
			continue;
		}

		std::wstring key = stem;
		override_loader::OverrideRecord rec;
		rec.key = key;
		rec.bin = std::move(bin);
		if (names)
			rec.tool_names = *names;
		g_overrides[key] = std::move(rec);
		log_info("override: registered '%ls'  bin=%zu names=%zu mod='%s'",
		         key.c_str(), g_overrides[key].bin.size(),
		         g_overrides[key].tool_names.size(), lm.cfg.name.c_str());
	} while (FindNextFileW(h, &fd));
	FindClose(h);
}

static void scan_mod(const LoadedMod &lm)
{
	std::wstring ov_dir;
	for (const wchar_t *v : {L"\\overrides", L"\\Overrides"})
	{
		std::wstring c = lm.dir_w + v;
		if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES)
		{
			ov_dir = c;
			break;
		}
	}
	if (ov_dir.empty())
		return;

	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((ov_dir + L"\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;
	do
	{
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
			continue;
		if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
			continue;
		scan_package_dir(ov_dir + L"\\" + fd.cFileName, fd.cFileName, lm);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
}

namespace override_loader
{
	void discover(const std::vector<LoadedMod> &mods)
	{
		for (const auto &lm : mods)
			scan_mod(lm);
		log_info("override_loader: %zu override(s) discovered",
		         g_overrides.size());
	}

	void install_hooks()
	{
		if (g_overrides.empty())
		{
			log_info("override_loader: no overrides — hook skipped");
			return;
		}

		if (!ue3().ok || !ue3().Preload)
		{
			log_err("override_loader: layout unresolved (Preload=%p ok=%d) — "
			        "overrides disabled",
			        ue3().Preload, (int)ue3().ok);
			return;
		}

		void *addr = ue3().Preload;
		hook::add(addr, reinterpret_cast<void *>(&hooked_Preload),
		          reinterpret_cast<void **>(&g_orig_Preload));

		log_info("override_loader: Preload hook at %p  Serialize_slot=%d  "
		         "GSerialObj=%p  (%zu override(s))",
		         addr, uobj_serialize_slot(),
		         static_cast<void *>(ue3().GSerializedObject),
		         g_overrides.size());
	}

	OverrideRecord *find(const std::wstring &key)
	{
		auto it = g_overrides.find(key);
		return it != g_overrides.end() ? &it->second : nullptr;
	}

	size_t count() { return g_overrides.size(); }
}  // namespace override_loader
