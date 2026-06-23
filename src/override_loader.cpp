#define WIN32_LEAN_AND_MEAN
#include "override_loader.hpp"
#include "hook.hpp"
#include "linker_layout.hpp"
#include "logs.hpp"
#include "pattern_scanner.hpp"
#include "ue3_types.hpp"
#include "util.hpp"
#include "utypes/ULinker.hpp"
#include "utypes/ULinkerLoad_Preload.hpp"
#include <cstring>
#include <psapi.h>
#include <sstream>
#include <unordered_map>
#include <windows.h>

static constexpr int kVT_Serialize = FArchiveVtSlot::Serialize;
static constexpr int kVT_Tell = FArchiveVtSlot::Tell;
static constexpr int kVT_TotalSize = FArchiveVtSlot::TotalSize;
static constexpr int kVT_Seek = FArchiveVtSlot::Seek;
static constexpr int kVT_Precache = FArchiveVtSlot::Precache;
static constexpr int kVT_IsError = FArchiveVtSlot::GetError;
static constexpr int kVT_SLOTS = FArchiveVtSlot::Count;

static void copy_live_version(FArchiveData *dst, void *linker)
{
	const auto *src = reinterpret_cast<const FArchiveData *>(
	    static_cast<const uint8_t *>(linker_farchive(linker)) + sizeof(void *));
	dst->ArVer = src->ArVer;
	dst->ArNetVer = src->ArNetVer;
	dst->ArLicenseeVer = src->ArLicenseeVer;
	dst->ArIsLoading = 1;
}

struct BufReader
{
	void *vt;
	FArchiveData ar;
	const uint8_t *data;
	size_t size;
	size_t pos;
	int32_t serial_off;
	bool anchored;
};

static_assert(offsetof(BufReader, vt) == 0);
static_assert(offsetof(BufReader, ar) == 8);
static_assert(offsetof(BufReader, data) == 136);
static_assert(offsetof(BufReader, ar.ArVer) == 8);

static void *g_buf_vt[kVT_SLOTS];
static bool g_buf_vt_ready = false;

static void __cdecl br_Serialize(BufReader *self, void *dst, int32_t len)
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

static int32_t __cdecl br_Tell(BufReader *self)
{
	return self->anchored ? self->serial_off + static_cast<int32_t>(self->pos)
	                      : static_cast<int32_t>(self->pos);
}

static void __cdecl br_Seek(BufReader *self, int32_t pos)
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

static int32_t __cdecl br_TotalSize(BufReader *self)
{
	return static_cast<int32_t>(self->size);
}

static int32_t __cdecl br_Precache(BufReader *, int32_t, int32_t) { return 1; }

static int32_t __cdecl br_IsError(BufReader *) { return 0; }

static void build_buf_vt(void **real_vt)
{
	memcpy(g_buf_vt, real_vt, kVT_SLOTS * sizeof(void *));
	g_buf_vt[kVT_Serialize] = reinterpret_cast<void *>(&br_Serialize);
	g_buf_vt[kVT_Tell] = reinterpret_cast<void *>(&br_Tell);
	g_buf_vt[kVT_TotalSize] = reinterpret_cast<void *>(&br_TotalSize);
	g_buf_vt[kVT_Seek] = reinterpret_cast<void *>(&br_Seek);
	g_buf_vt[kVT_Precache] = reinterpret_cast<void *>(&br_Precache);
	g_buf_vt[kVT_IsError] = reinterpret_cast<void *>(&br_IsError);
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
};

static thread_local FNamePatchCtx tl_fn{};

static void *__cdecl fname_remap_thunk(void *fa, void *fname_out)
{
	const auto &ll = linker_layout();
	{
		void *lb = static_cast<uint8_t *>(fa) - ll.farchive_off;
		void *cur = *linker_loader_ptr(lb);
		if (cur != tl_fn.active_br)
		{
			using FnT = void *(__cdecl *)(void *, void *);
			return reinterpret_cast<FnT>(tl_fn.orig_fn)(fa, fname_out);
		}
	}

	int32_t raw[2] = {};
	auto *br = static_cast<BufReader *>(tl_fn.active_br);
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

	int32_t live = raw[0];
	if (raw[0] >= 0 && tl_fn.remap &&
	    static_cast<size_t>(raw[0]) < tl_fn.remap_size)
	{
		int32_t m = tl_fn.remap[raw[0]];
		if (m >= 0)
			live = m;
		else
			log_warn("override: fname_remap: tool_idx=%d unmapped", raw[0]);
	}

	void *lb2 = static_cast<uint8_t *>(fa) - ll.farchive_off;
	const TArray<FName> *nm = linker_namemap(lb2);

	int32_t raw_name_idx = live;
	if (nm->Data && live >= 0 && live < nm->Num)
		raw_name_idx = nm->Data[live].Index;

	int32_t *out = static_cast<int32_t *>(fname_out);
	out[0] = raw_name_idx;
	out[1] = raw[1];
	return fa;
}

static void install_fname_patch(void *linker,
                                const override_loader::OverrideRecord &rec)
{
	const auto &ll = linker_layout();
	uint8_t *fa = static_cast<uint8_t *>(linker) + ll.farchive_off;
	void **orig = *reinterpret_cast<void ***>(fa);

	void **pv = new void *[kVT_SLOTS];
	memcpy(pv, orig, kVT_SLOTS * sizeof(void *));
	pv[ll.fname_slot] = reinterpret_cast<void *>(&fname_remap_thunk);

	tl_fn = {rec.name_remap.data(),
	         rec.name_remap.size(),
	         orig[ll.fname_slot],
	         orig,
	         pv,
	         nullptr};

	*reinterpret_cast<void ***>(fa) = pv;
	log_info("override: fname patch fa=%p orig_vt=%p pv=%p", (void *)fa,
	         (void *)orig, (void *)pv);
}

static void remove_fname_patch(void *linker)
{
	const auto &ll = linker_layout();
	uint8_t *fa = static_cast<uint8_t *>(linker) + ll.farchive_off;
	if (!tl_fn.patched_vt)
		return;
	*reinterpret_cast<void ***>(fa) = tl_fn.orig_vt;
	delete[] tl_fn.patched_vt;
	tl_fn = {};
	log_info("override: fname patch removed fa=%p", (void *)fa);
}

static bool safe_read(const void *p, size_t n)
{
	return p && !IsBadReadPtr(p, n);
}

static std::wstring fname_str(int32_t gidx)
{
	const auto *names = ue3().FNameNames;
	if (!names || gidx < 0 || gidx >= names->Num)
		return {};
	void *entry = names->Data[gidx];
	if (!entry || !safe_read(entry, ue3().name_layout.str_off + 4))
		return {};
	if (ue3().name_layout.is_unicode(entry))
	{
		const wchar_t *s = ue3().name_layout.uni(entry);
		return safe_read(s, 2) ? std::wstring(s) : std::wstring{};
	}
	const char *s = ue3().name_layout.ansi(entry);
	return safe_read(s, 1) ? to_wide(std::string(s)) : std::wstring{};
}

static void debug_dump_chain(const void *obj)
{
	const void *cur = obj;
	for (int d = 0; d < 8 && cur; ++d)
	{
		if (!safe_read(cur, sizeof(UObject_Mirror)))
		{
			log_info("  [%d] ptr=%p UNREADABLE", d, cur);
			break;
		}
		const FName nm = uobj_name(cur);
		const void *outer = uobj_outer(cur);
		std::wstring s = (nm.Index >= 0 && nm.Index < ue3().FNameNames->Num)
		                     ? fname_str(nm.Index)
		                     : L"<oob>";
		log_info(
		    "  [%d] ptr=%p outer=%p name.Index=%d name.Number=%d str='%ls'", d,
		    cur, outer, nm.Index, nm.Number, s.c_str());
		cur = outer;
	}
}

static std::wstring get_uobj_path(const void *obj)
{
	if (!obj || !ue3().FNameNames || !safe_read(obj, sizeof(UObject_Mirror)))
		return {};

	std::vector<std::wstring> parts;
	const void *cur = obj;
	for (int d = 0; d < 8 && cur; ++d)
	{
		if (!safe_read(cur, sizeof(UObject_Mirror)))
			break;
		const FName nm = uobj_name(cur);
		if (nm.Index < 0 || nm.Index >= ue3().FNameNames->Num)
			break;
		std::wstring seg = fname_str(nm.Index);
		if (seg.empty())
			break;
		parts.push_back(seg);
		cur = uobj_outer(cur);
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

static void build_name_remap(override_loader::OverrideRecord &rec, void *linker)
{
	rec.name_remap_ready = true;
	if (rec.tool_names.empty())
		return;

	const TArray<FName> *nm = linker_namemap(linker);
	log_info("override: build_name_remap '%ls' nm=%p Num=%d", rec.key.c_str(),
	         (void *)nm->Data, nm->Num);

	if (!nm->Data || nm->Num <= 0 || nm->Num > 1000000 ||
	    IsBadReadPtr(nm->Data, static_cast<size_t>(nm->Num) * sizeof(FName)))
	{
		log_warn("override: build_name_remap '%ls' — NameMap invalid",
		         rec.key.c_str());
		return;
	}

	rec.name_remap.assign(rec.tool_names.size(), -1);
	for (int32_t li = 0; li < nm->Num; ++li)
	{
		const int32_t gidx = fname_entry_index(nm->Data[li]);
		if (gidx < 0 || gidx >= ue3().FNameNames->Num)
			continue;
		const std::wstring ws = fname_str(gidx);
		if (ws.empty())
			continue;
		const std::string n(ws.begin(), ws.end());
		for (size_t ti = 0; ti < rec.tool_names.size(); ++ti)
			if (rec.tool_names[ti] == n && rec.name_remap[ti] < 0)
				rec.name_remap[ti] = li;
	}

	size_t mapped = 0;
	for (int32_t v : rec.name_remap)
		if (v >= 0)
			++mapped;
	log_info("override: name_remap '%ls' %zu/%zu resolved", rec.key.c_str(),
	         mapped, rec.tool_names.size());
}

using PreloadFn = void(__cdecl *)(ULinkerLoad_Mirror *, UObject_Mirror *);
static PreloadFn g_orig_Preload = nullptr;

static void call_object_serialize(void *obj, void *linker)
{
	const auto &ll = linker_layout();
	void *farchive = static_cast<uint8_t *>(linker) + ll.farchive_off;

	if (ll.uobj_serialize_vtslot < 0)
	{

		log_warn("override: Serialize slot unknown — should not reach here");
		return;
	}

	void *prev_serial = nullptr;
	if (ll.g_serialized_obj)
	{
		prev_serial = *ll.g_serialized_obj;
		*ll.g_serialized_obj = obj;
	}

	using SerializeFn = void(__cdecl *)(void *, void *);
	void **vt = *static_cast<void ***>(obj);
	auto serialize =
	    reinterpret_cast<SerializeFn>(vt[ll.uobj_serialize_vtslot]);
	serialize(obj, farchive);

	if (ll.g_serialized_obj)
		*ll.g_serialized_obj = prev_serial;
}

static void do_override_preload(ULinkerLoad_Mirror *linker, UObject_Mirror *obj,
                                const FObjectExport_Mirror &exp,
                                override_loader::OverrideRecord &rec)
{
	const auto &ll = linker_layout();
	const bool has_remap = !rec.name_remap.empty();
	const bool vtslot_known = ll.uobj_serialize_vtslot >= 0;

	if (uobj_has_flag(obj, kRF_ClassDefaultObject) || !vtslot_known)
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
		install_fname_patch(linker, rec);
		tl_fn.active_br = br_ptr;
	}

	br_Seek(&br, exp.SerialOffset);

	uobj_clear_flag(obj, kRF_NeedLoad);

	call_object_serialize(obj, linker);

	if (has_remap)
		remove_fname_patch(linker);

	*linker_loader_ptr(linker) = real_loader;
	*linker_original_loader_ptr(linker) = real_original;

	log_info("override: full-port '%ls' consumed=%zu/%zu bytes",
	         rec.key.c_str(), br.pos, br.size);
}

static const char *kPat_Preload =
    "48 8b c4 55 56 57 41 54 41 55 41 56 41 57 48 81 ec a0 00 00 00 48 c7 44 "
    "24 60 fe ff ff ff";

static void __cdecl hooked_Preload(ULinkerLoad_Mirror *linker,
                                   UObject_Mirror *obj)
{
	if (!linker_layout().valid || !linker || !obj || !ue3().FNameNames)
	{
		if (linker && g_orig_Preload)
			g_orig_Preload(linker, obj);
		return;
	}

	if (!safe_read(obj, sizeof(UObject_Mirror)))
	{
		g_orig_Preload(linker, obj);
		return;
	}

	if (!uobj_has_flag(obj, kRF_NeedLoad))
	{
		g_orig_Preload(linker, obj);
		return;
	}

	log_info("obj=%p _Linker=%p linker=%p outer=%p class=%p", obj, obj->_Linker,
	         linker, obj->Outer, obj->Class);
	if (obj->_Linker != linker)
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

	log_info("override: Preload '%ls' (%zu bytes)", path.c_str(),
	         rec->bin.size());

	if (!uobj_has_flag(obj, kRF_NeedLoad))
	{
		log_info("override: '%ls' RF_NeedLoad already clear after re-check",
		         path.c_str());
		return;
	}

	const int32_t idx = obj->_LinkerIndex;
	const FObjectExport_Mirror *exp = linker_get_export(linker, idx);

	if (!exp)
	{
		log_warn("override: '%ls' — bad _LinkerIndex %td, passing through",
		         path.c_str(), idx);
		g_orig_Preload(linker, obj);
		return;
	}

	if (exp->_Object && exp->_Object != obj)
	{
		log_warn("override: '%ls' Export._Object mismatch (%p vs %p)",
		         path.c_str(), exp->_Object, obj);
	}

	if (exp->ExportFlags & kEF_ScriptPatcherExport)
	{
		log_warn("override: '%ls' has EF_ScriptPatcherExport — passing through",
		         path.c_str());
		g_orig_Preload(linker, obj);
		return;
	}

	do_override_preload(linker, obj, *exp, *rec);
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

static std::wstring find_namemap_for_key(const std::wstring &key,
                                         const std::wstring &ov_dir)
{
	std::wstring k = key;
	while (true)
	{
		std::wstring p = ov_dir + L"\\" + k + L".namemap";
		if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES)
			return p;
		auto dot = k.rfind(L'.');
		if (dot == std::wstring::npos)
			break;
		k = k.substr(0, dot);
	}
	return {};
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
	HANDLE h = FindFirstFileW((ov_dir + L"\\*.bin").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;

	do
	{
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		std::wstring key = file_stem(fd.cFileName);
		if (key.empty())
			continue;

		auto bin = read_file(ov_dir + L"\\" + fd.cFileName);
		if (bin.empty())
		{
			log_warn("override: read failed '%ls'", fd.cFileName);
			continue;
		}

		std::wstring nm_path = find_namemap_for_key(key, ov_dir);

		override_loader::OverrideRecord rec;
		rec.key = key;
		rec.bin = std::move(bin);
		rec.upk_version =
		    lm.cfg.upk_version ? int32_t(lm.cfg.upk_version) : 801;
		rec.license_version =
		    lm.cfg.license_version ? int32_t(lm.cfg.license_version) : 0;

		if (!nm_path.empty())
			rec.tool_names = parse_namemap(nm_path);
		else
			log_warn("override: no .namemap for '%ls' — FName remap disabled",
			         key.c_str());

		g_overrides[key] = std::move(rec);
		log_info("override: registered '%ls'  bin=%zu bytes  namemap=%zu names "
		         " mod='%s'",
		         key.c_str(), g_overrides[key].bin.size(),
		         g_overrides[key].tool_names.size(), lm.cfg.name.c_str());

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

		void *addr = FindPatternString(GetModuleHandleW(nullptr), kPat_Preload);
		if (!addr)
		{
			log_err("override_loader: Preload pattern NOT FOUND — overrides "
			        "disabled");
			return;
		}

		LinkerLayout ll;
		resolve_linker_layout(ll, nullptr, addr);

		hook::add(addr, reinterpret_cast<void *>(&hooked_Preload),
		          reinterpret_cast<void **>(&g_orig_Preload));

		log_info("override_loader: Preload hook at %p  Serialize_slot=%d  "
		         "GSerialObj=%p  (%zu override(s))",
		         addr, ll.uobj_serialize_vtslot,
		         static_cast<void *>(ll.g_serialized_obj), g_overrides.size());
	}

	OverrideRecord *find(const std::wstring &key)
	{
		auto it = g_overrides.find(key);
		return it != g_overrides.end() ? &it->second : nullptr;
	}

	size_t count() { return g_overrides.size(); }
}  // namespace override_loader
