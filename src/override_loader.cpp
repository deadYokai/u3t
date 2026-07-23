#define WIN32_LEAN_AND_MEAN
#include "override_loader.hpp"
#include "hook.hpp"
#include "logs.hpp"
#include "ue3_api.hpp"
#include "ue3_layout.hpp"
#include "ue3_patch.hpp"
#include "util.hpp"
#include <algorithm>
#include <cstring>
#include <memory>
#include <psapi.h>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <windows.h>

#include "lua_host.hpp"

static constexpr int kVT_MAX = 64;

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
		FNameStack *existing = reinterpret_cast<FNameStack *>(name_map->Data);
		int existing_num = name_map->Num;

		std::vector<FNameStack> to_add;
		rec.name_map_final.resize(rec.name_remap.size());

		for (size_t i = 0; i < rec.name_remap.size(); ++i)
		{
			FNameStack fname{rec.name_remap[i], 0};
			bool found = false;
			for (int j = 0; j < existing_num; ++j)
			{
				if (memcmp(&existing[j], &fname, sizeof(FNameStack)) == 0)
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

struct NewExportSpec
{
	int32_t index = -1;
	std::wstring path;       // full object path; also the override key
	std::string class_name;  // informational only
	int32_t class_index = 0;
	int32_t outer_index = 0;
	int32_t super_index = 0;
	int32_t archetype_index = 0;
	uint64_t object_flags = 0;
	uint32_t export_flags = 0;
	int32_t serial_size = 0;
	std::wstring bin_name;
};

struct PendingExports
{
	std::wstring pkg;
	std::vector<NewExportSpec> specs;
	bool injected = false;
	bool failed = false;

	int32_t expected_base() const
	{
		return specs.empty() ? -1 : specs.front().index - 1;
	}
};

static std::wstring lower_w(std::wstring s)
{
	std::transform(s.begin(), s.end(), s.begin(),
	               [](wchar_t c) { return (wchar_t)towlower(c); });
	return s;
}

static inline void orig_preload(void *linker, void *obj)
{
	if (linker && g_orig_Preload)
		g_orig_Preload(linker_farchive(linker), obj);
}

static std::unordered_map<std::wstring, PendingExports> g_new_exports;
static int g_preload_depth = 0;

enum PendingState
{
	kPending_None = 0,
	kPending_Ready = 1,
	kPending_Waiting = 2,
	kPending_Failed = 3,
};

static std::wstring linker_root_name(void *linker)
{
	if (!linker)
		return {};
	void *root = linker_root(linker);
	if (!root || !safe_read(root, ue3().sizeof_UObject))
		return {};
	return fname_str(uobj_name_index(root));
}

static PendingState pending_state_for_linker(void *linker)
{
	if (g_new_exports.empty() || !linker)
		return kPending_None;
	const std::wstring root = linker_root_name(linker);
	if (root.empty())
		return kPending_None;
	auto it = g_new_exports.find(lower_w(root));
	if (it == g_new_exports.end())
		return kPending_None;
	if (it->second.failed)
		return kPending_Failed;
	return it->second.injected ? kPending_Ready : kPending_Waiting;
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

	if (!rec.pkg.empty())
	{
		const std::wstring root = linker_root_name(linker);
		if (lower_w(root) != lower_w(rec.pkg))
		{
			log_warn("override: '%ls' was dumped from '%ls' but this object's "
			         "linker root is '%ls' (ExportMap=%d NameMap=%d) — passing "
			         "through",
			         rec.key.c_str(), rec.pkg.c_str(),
			         root.empty() ? L"<unknown>" : root.c_str(),
			         linker_exportmap(linker).num, linker_namemap(linker).num);
			orig_preload(linker, obj);
			return;
		}
	}

	switch (pending_state_for_linker(linker))
	{
		case kPending_Waiting:
		case kPending_Failed:
			log_warn("override: '%ls' needs new exports that are not injected "
			         "into '%ls' — passing through",
			         rec.key.c_str(), rec.pkg.c_str());
			orig_preload(linker, obj);
			return;
		default:
			break;
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

static bool parse_num(const std::string &s, int64_t &out)
{
	size_t i = 0;
	bool neg = false;
	if (i < s.size() && (s[i] == '-' || s[i] == '+'))
		neg = (s[i++] == '-');
	int base = 10;
	if (s.size() - i > 2 && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X'))
	{
		base = 16;
		i += 2;
	}
	if (i >= s.size())
		return false;
	uint64_t v = 0;
	for (; i < s.size(); ++i)
	{
		const char c = s[i];
		int d;
		if (c >= '0' && c <= '9')
			d = c - '0';
		else if (c >= 'a' && c <= 'f')
			d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			d = c - 'A' + 10;
		else
			return false;
		if (d >= base)
			return false;
		v = v * base + static_cast<uint64_t>(d);
	}
	out = neg ? -static_cast<int64_t>(v) : static_cast<int64_t>(v);
	return true;
}

static std::vector<std::string> split_tabs(const std::string &line)
{
	std::vector<std::string> out;
	size_t start = 0;
	for (;;)
	{
		const size_t t = line.find('\t', start);
		if (t == std::string::npos)
		{
			out.push_back(line.substr(start));
			return out;
		}
		out.push_back(line.substr(start, t - start));
		start = t + 1;
	}
}

static void parse_newexports(const std::wstring &file,
                             const std::wstring &pkg_name)
{
	auto raw = read_file(file);
	if (raw.empty())
	{
		log_warn("newexports: read failed '%ls'", file.c_str());
		return;
	}

	PendingExports pe;
	pe.pkg = pkg_name;

	std::string text(raw.begin(), raw.end());
	std::istringstream ss(text);
	std::string line;
	int lineno = 0;
	while (std::getline(ss, line))
	{
		++lineno;
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty() || line[0] == '#')
			continue;

		auto col = split_tabs(line);
		if (col.size() < 11)
		{
			log_warn("newexports: %ls:%d — expected 11 columns, got %zu",
			         file.c_str(), lineno, col.size());
			continue;
		}

		int64_t v[8] = {};
		static const int kNumCols[8] = {0, 3, 4, 5, 6, 7, 8, 9};
		bool ok = true;
		for (int i = 0; i < 8 && ok; ++i)
			ok = parse_num(col[kNumCols[i]], v[i]);
		if (!ok)
		{
			log_warn("newexports: %ls:%d — malformed numeric field",
			         file.c_str(), lineno);
			continue;
		}

		NewExportSpec s;
		s.index = static_cast<int32_t>(v[0]);
		s.path = to_wide(col[1]);
		s.class_name = col[2];
		s.class_index = static_cast<int32_t>(v[1]);
		s.outer_index = static_cast<int32_t>(v[2]);
		s.super_index = static_cast<int32_t>(v[3]);
		s.archetype_index = static_cast<int32_t>(v[4]);
		s.object_flags = static_cast<uint64_t>(v[5]);
		s.export_flags = static_cast<uint32_t>(v[6]);
		s.serial_size = static_cast<int32_t>(v[7]);
		s.bin_name = to_wide(col[10]);

		if (s.path.empty() || s.index < 0)
		{
			log_warn("newexports: %ls:%d — bad index/path", file.c_str(),
			         lineno);
			continue;
		}
		pe.specs.push_back(std::move(s));
	}

	if (pe.specs.empty())
		return;

	std::sort(pe.specs.begin(), pe.specs.end(),
	          [](const NewExportSpec &a, const NewExportSpec &b)
	          { return a.index < b.index; });

	log_info("newexports: '%ls' — %zu new export(s) queued", pkg_name.c_str(),
	         pe.specs.size());
	for (const auto &s : pe.specs)
		log_info("newexports:   [%d] '%ls' class=%s cls_idx=%d outer=%d "
		         "flags=0x%016llX ef=0x%08X size=%d",
		         s.index, s.path.c_str(), s.class_name.c_str(), s.class_index,
		         s.outer_index, (unsigned long long)s.object_flags,
		         s.export_flags, s.serial_size);

	g_new_exports[lower_w(pkg_name)] = std::move(pe);
}

static bool make_object_fname(const std::wstring &leaf, FNameStack &out)
{
	using FNameInitFn = void(UE3_THISCALL *)(void *, const wchar_t *, int32_t,
	                                         int32_t, int32_t);
	auto fni = reinterpret_cast<FNameInitFn>(ue3().FNameInit);
	if (!fni)
		return false;
	out = FNameStack{};
	fni(&out, leaf.c_str(), 0, 1, 1);
	return true;
}

static bool exports_all_created(void *linker)
{
	TArrayView em = linker_exportmap(linker);
	if (!em.data || em.num <= 0)
		return false;
	for (int i = 0; i < em.num; ++i)
	{
		void *e = em.data + static_cast<ptrdiff_t>(i) * ue3().exp_stride;
		if (!safe_read(e, ue3().exp_stride) || !exp_object(e))
			return false;
	}
	return true;
}

static void inject_new_exports(void *linker, PendingExports &pe)
{
	UE3Layout &L = ue3();
	if (!L.ArrayRealloc || !L.FNameInit || !L.exp_stride)
	{
		log_err("newexports: '%ls' — ArrayRealloc/FNameInit unresolved, "
		        "injection disabled",
		        pe.pkg.c_str());
		pe.failed = true;
		return;
	}

	const int base = linker_exportmap(linker).num;
	for (size_t i = 0; i < pe.specs.size(); ++i)
	{
		const int want_slot = base + static_cast<int>(i);
		const int have_slot = pe.specs[i].index - 1;
		if (have_slot != want_slot)
		{
			log_err("newexports: '%ls' wants slot %d (PACKAGE_INDEX %d) but "
			        "would land at %d — refusing to inject, .bin references "
			        "would be wrong",
			        pe.specs[i].path.c_str(), have_slot, pe.specs[i].index,
			        want_slot);
			pe.failed = true;
			return;
		}
	}

	void *first = ue3_append_exports(linker, static_cast<int>(pe.specs.size()));
	if (!first)
	{
		log_err("newexports: '%ls' — ExportMap append failed", pe.pkg.c_str());
		pe.failed = true;
		return;
	}

	for (size_t i = 0; i < pe.specs.size(); ++i)
	{
		const NewExportSpec &s = pe.specs[i];
		uint8_t *e = static_cast<uint8_t *>(first) + i * L.exp_stride;

		const size_t dot = s.path.rfind(L'.');
		const std::wstring leaf =
		    dot == std::wstring::npos ? s.path : s.path.substr(dot + 1);

		FNameStack fn{};
		if (!make_object_fname(leaf, fn))
		{
			log_err("newexports: FName init failed for '%ls'", leaf.c_str());
			pe.failed = true;
			return;
		}
		memcpy(e + L.e_ObjectName, &fn, sizeof(fn));

		ue3raw::wr_i32(e, L.e_OuterIndex, s.outer_index);
		ue3raw::wr_i32(e, L.e_ClassIndex, s.class_index);
		ue3raw::wr_i32(e, L.e_SuperIndex, s.super_index);
		ue3raw::wr_i32(e, L.e_ArchetypeIndex, s.archetype_index);
		ue3raw::wr_u64(e, L.e_ObjectFlags, s.object_flags);
		ue3raw::wr_i32(e, L.e_SerialSize, s.serial_size);
		ue3raw::wr_i32(e, L.e_SerialOffset, 0);
		ue3raw::wr_ptr(e, L.e_Object, nullptr);
		ue3raw::wr_i32(e, L.e_iHashNext, -1);  // INDEX_NONE, not hashed

		const uint32_t ef =
		    s.export_flags & ~(EF_ForcedExport | EF_ScriptPatcherExport);
		if (ef != s.export_flags)
			log_warn("newexports: '%ls' export_flags 0x%08X -> 0x%08X "
			         "(stripped EF_ForcedExport/EF_ScriptPatcherExport)",
			         s.path.c_str(), s.export_flags, ef);
		ue3raw::wr_u32(e, L.e_ExportFlags, ef);

		if (!override_loader::find(s.path))
			log_warn("newexports: no .bin registered for '%ls' — the object "
			         "will be constructed but serialized from garbage",
			         s.path.c_str());

		log_info("newexports: injected [%d] '%ls' name=(%d,%d) outer=%d "
		         "class=%d size=%d",
		         base + (int)i, s.path.c_str(), fn.Index, fn.Number,
		         s.outer_index, s.class_index, s.serial_size);
	}

	log_info("newexports: '%ls' ExportMap %d -> %d", pe.pkg.c_str(), base,
	         linker_exportmap(linker).num);

	pe.injected = true;
}

static PendingExports *pending_for_linker(void *linker)
{
	if (g_new_exports.empty() || !linker)
		return nullptr;
	void *root = linker_root(linker);
	if (!root || !safe_read(root, ue3().sizeof_UObject))
	{
		log_warn("newexports: linker=%p LinkerRoot unreadable (%p)", linker,
		         root);
		return nullptr;
	}
	const std::wstring pkg = fname_str(uobj_name_index(root));
	auto it = g_new_exports.find(lower_w(pkg));
	if (it == g_new_exports.end())
		return nullptr;
	return &it->second;
}

static void try_inject_for_linker(void *linker, const char *why)
{
	PendingExports *pe = pending_for_linker(linker);
	if (!pe || pe->injected || pe->failed)
		return;

	const int base = linker_exportmap(linker).num;
	const int want = pe->expected_base();
	if (base != want)
	{
		log_info("newexports: '%ls' deferred at %s — ExportMap.Num()=%d, "
		         "expected %d",
		         pe->pkg.c_str(), why, base, want);
		return;
	}

	log_info("newexports: '%ls' injecting at %s (base=%d)", pe->pkg.c_str(),
	         why, base);
	inject_new_exports(linker, *pe);
}

#ifdef _WIN64
#define UE3_CDECL
#else
#define UE3_CDECL __cdecl
#endif

using GetPackageLinkerFn = void *(UE3_CDECL *)(void *, const wchar_t *,
                                               uint32_t, void *, void *);
static GetPackageLinkerFn g_orig_GetPackageLinker = nullptr;

static void *UE3_CDECL hooked_GetPackageLinker(void *InOuter,
                                               const wchar_t *Filename,
                                               uint32_t LoadFlags,
                                               void *Sandbox,
                                               void *CompatibleGuid)
{
	void *linker = g_orig_GetPackageLinker(InOuter, Filename, LoadFlags,
	                                       Sandbox, CompatibleGuid);
	if (linker && ue3().ok && !g_new_exports.empty())
		try_inject_for_linker(linker, "GetPackageLinker");
	return linker;
}

static void maybe_inject_new_exports(void *linker)
{
	if (g_preload_depth != 1 || !exports_all_created(linker))
		return;
	try_inject_for_linker(linker, "Preload/fallback");
}

static void preload_call(void *farchive, void *obj)
{
	struct DepthGuard
	{
		DepthGuard() { ++g_preload_depth; }

		~DepthGuard() { --g_preload_depth; }
	} depth_guard;

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

	maybe_inject_new_exports(linker);

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

static void __fastcall hooked_Preload(void *farchive, UE3_EDX void *obj)
{
	const bool needed = obj && uobj_has_flag(obj, RF_NeedLoad);

	preload_call(farchive, obj);
	if (needed)
		lua_host::notify_preloaded(obj);
}

static std::unordered_map<std::wstring, override_loader::OverrideRecord>
    g_overrides;

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

	{
		std::wstring ne = pkg_dir + L"\\" + pkg_name + L".newexports";
		if (GetFileAttributesW(ne.c_str()) != INVALID_FILE_ATTRIBUTES)
			parse_newexports(ne, pkg_name);
	}

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
		rec.pkg = pkg_name;
		rec.bin = std::move(bin);
		if (names)
			rec.tool_names = *names;
		g_overrides[key] = std::move(rec);
		log_info("override: registered '%ls'  pkg='%ls' bin=%zu names=%zu "
		         "mod='%s'",
		         key.c_str(), pkg_name.c_str(), g_overrides[key].bin.size(),
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
		size_t nexp = 0;
		for (const auto &kv : g_new_exports)
			nexp += kv.second.specs.size();
		log_info("override_loader: %zu override(s), %zu new export(s) "
		         "discovered",
		         g_overrides.size(), nexp);
	}

	void install_hooks()
	{
		if (g_overrides.empty() && g_new_exports.empty())
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

		if (!g_new_exports.empty())
		{
			if (ue3().GetPackageLinker)
			{
				hook::add(ue3().GetPackageLinker,
				          reinterpret_cast<void *>(&hooked_GetPackageLinker),
				          reinterpret_cast<void **>(&g_orig_GetPackageLinker));
				log_info("newexports: GetPackageLinker hook at %p",
				         ue3().GetPackageLinker);
			}
			else
			{
				log_warn("newexports: GetPackageLinker unresolved — falling "
				         "back to the Preload gate, injection may be too late");
			}
		}

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
