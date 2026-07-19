#include "util.hpp"
#define WIN32_LEAN_AND_MEAN
#include "addr_cache.hpp"
#include "logs.hpp"
#include "ue3_layout.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>

namespace
{
	constexpr int kCacheVersion = 1;

	struct Entry
	{
		std::string key;
		std::string value;
	};

	std::vector<Entry> g_entries;
	std::wstring g_path;
	uint8_t *g_base = nullptr;
	size_t g_image_size = 0;

	bool g_ready = false;
	bool g_valid = false;
	bool g_dirty = false;

	std::string *find_entry(const char *key)
	{
		for (auto &e : g_entries)
			if (e.key == key)
				return &e.value;
		return nullptr;
	}

	void set_entry(const char *key, const std::string &v)
	{
		if (std::string *slot = find_entry(key))
		{
			if (*slot != v)
			{
				*slot = v;
				g_dirty = true;
			}
			return;
		}
		g_entries.push_back({key, v});
		g_dirty = true;
	}

	struct ImageStamp
	{
		uint32_t timestamp = 0;
		uint32_t size = 0;
		uint32_t checksum = 0;
		uint32_t entry = 0;
		bool ok = false;
	};

	ImageStamp stamp_of(uint8_t *base)
	{
		ImageStamp s;
		if (!base)
			return s;

		auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return s;

		auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return s;

		s.timestamp = nt->FileHeader.TimeDateStamp;
		s.size = nt->OptionalHeader.SizeOfImage;
		s.checksum = nt->OptionalHeader.CheckSum;
		s.entry = nt->OptionalHeader.AddressOfEntryPoint;
		s.ok = true;
		return s;
	}

	std::string hex_u64(uint64_t v)
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)v);
		return buf;
	}

	std::string dec_i64(int64_t v)
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "%lld", (long long)v);
		return buf;
	}

	bool read_file(const std::wstring &path, std::string &out)
	{
		HANDLE h =
		    CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
		                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return false;

		LARGE_INTEGER sz{};
		if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 1 << 20)
		{
			CloseHandle(h);
			return false;
		}

		out.resize(static_cast<size_t>(sz.QuadPart));
		DWORD got = 0;
		BOOL rc = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got,
		                   nullptr);
		CloseHandle(h);
		if (!rc)
			return false;

		out.resize(got);
		return true;
	}

	bool write_file(const std::wstring &path, const std::string &text)
	{
		HANDLE h =
		    CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
		                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
		{
			log_warn("addrcache: cannot write %ls (err %lu)", path.c_str(),
			         GetLastError());
			return false;
		}

		DWORD wrote = 0;
		BOOL rc = WriteFile(h, text.data(), static_cast<DWORD>(text.size()),
		                    &wrote, nullptr);
		CloseHandle(h);
		return rc && wrote == text.size();
	}

	void parse_into(const std::string &text, std::vector<Entry> &out)
	{
		size_t i = 0;
		while (i < text.size())
		{
			size_t eol = text.find('\n', i);
			if (eol == std::string::npos)
				eol = text.size();

			std::string line = text.substr(i, eol - i);
			i = eol + 1;

			if (size_t h = line.find('#'); h != std::string::npos)
				line.resize(h);

			size_t a = line.find_first_not_of(" \t\r");
			if (a == std::string::npos)
				continue;

			size_t sp = line.find_first_of(" \t", a);
			if (sp == std::string::npos)
				continue;

			std::string key = line.substr(a, sp - a);

			size_t b = line.find_first_not_of(" \t", sp);
			if (b == std::string::npos)
				continue;

			size_t e = line.find_last_not_of(" \t\r");
			std::string val = line.substr(b, e - b + 1);

			if (!key.empty() && !val.empty())
				out.push_back({key, val});
		}
	}

	void prime_header(const ImageStamp &st)
	{
		set_entry("cache.version", dec_i64(kCacheVersion));
		set_entry("cache.bits", dec_i64((int64_t)(sizeof(void *) * 8)));
		set_entry("image.timestamp", hex_u64(st.timestamp));
		set_entry("image.size", hex_u64(st.size));
		set_entry("image.checksum", hex_u64(st.checksum));
		set_entry("image.entry", hex_u64(st.entry));
	}

	bool header_matches(const std::vector<Entry> &loaded, const ImageStamp &st)
	{
		auto get = [&](const char *k) -> const std::string *
		{
			for (const auto &e : loaded)
				if (e.key == k)
					return &e.value;
			return nullptr;
		};
		auto eq = [&](const char *k, uint64_t want) -> bool
		{
			const std::string *v = get(k);
			if (!v)
				return false;
			return strtoull(v->c_str(), nullptr, 0) == want;
		};

		return eq("cache.version", kCacheVersion) &&
		       eq("cache.bits", sizeof(void *) * 8) &&
		       eq("image.timestamp", st.timestamp) &&
		       eq("image.size", st.size) && eq("image.checksum", st.checksum) &&
		       eq("image.entry", st.entry);
	}

	bool cmdline_disables_cache()
	{
		const wchar_t *cl = GetCommandLineW();
		return cl && wcsstr(cl, L"-cu3ml-noaddrcache") != nullptr;
	}
}  // namespace

namespace addr_cache
{

	void init()
	{
		if (g_ready)
			return;
		g_ready = true;

		g_base = reinterpret_cast<uint8_t *>(GetModuleHandleW(nullptr));
		ImageStamp st = stamp_of(g_base);
		g_image_size = st.size;
		g_path = get_mods_dir() + L"\\cu3ml.addrlist";

		if (!st.ok)
		{
			log_warn("addrcache: bad PE headers — cache disabled");
			g_ready = false;
			return;
		}

		if (cmdline_disables_cache())
		{
			log_info("addrcache: -cu3ml-noaddrcache given — forcing full scan");
			prime_header(st);
			return;
		}

		std::string text;
		if (!read_file(g_path, text))
		{
			log_info("addrcache: no %ls yet — full scan, will write one",
			         g_path.c_str());
			prime_header(st);
			return;
		}

		std::vector<Entry> loaded;
		parse_into(text, loaded);

		if (!header_matches(loaded, st))
		{
			log_warn("addrcache: %ls does not match this executable "
			         "(ts=0x%lX size=0x%lX sum=0x%lX) — discarding",
			         g_path.c_str(), (unsigned long)st.timestamp,
			         (unsigned long)st.size, (unsigned long)st.checksum);
			prime_header(st);
			return;
		}

		g_entries = std::move(loaded);
		g_valid = true;
		g_dirty = false;
		log_info("addrcache: loaded %zu entries from %ls (base=%p)",
		         g_entries.size(), g_path.c_str(), static_cast<void *>(g_base));
	}

	bool loaded() { return g_valid; }

	bool dirty() { return g_dirty; }

	bool get_ptr(const char *key, void *&out)
	{
		if (!g_valid)
			return false;

		const std::string *v = find_entry(key);
		if (!v)
			return false;

		if (*v == "null")
		{
			out = nullptr;
			return true;
		}

		char *end = nullptr;
		unsigned long long rva = strtoull(v->c_str(), &end, 0);
		if (end == v->c_str())
			return false;

		if (g_image_size && rva >= g_image_size)
		{
			log_warn("addrcache: %s rva 0x%llX outside image (0x%zX)", key, rva,
			         g_image_size);
			return false;
		}

		out = g_base + rva;
		return true;
	}

	void put_ptr(const char *key, const void *p)
	{
		if (!g_ready)
			return;

		if (!p)
		{
			set_entry(key, "null");
			return;
		}

		auto addr = reinterpret_cast<uintptr_t>(p);
		auto base = reinterpret_cast<uintptr_t>(g_base);
		if (addr < base || (g_image_size && addr - base >= g_image_size))
		{
			log_warn("addrcache: refusing to cache %s = %p (outside image)",
			         key, p);
			return;
		}

		set_entry(key, hex_u64(addr - base));
	}

	bool get_i64(const char *key, int64_t &out)
	{
		if (!g_valid)
			return false;

		const std::string *v = find_entry(key);
		if (!v)
			return false;

		char *end = nullptr;
		long long parsed = strtoll(v->c_str(), &end, 0);
		if (end == v->c_str())
			return false;

		out = static_cast<int64_t>(parsed);
		return true;
	}

	void put_i64(const char *key, int64_t v)
	{
		if (!g_ready)
			return;
		set_entry(key, dec_i64(v));
	}

	void save()
	{
		if (!g_ready || !g_dirty)
			return;

		std::string text;
		text += "# cu3ml address cache - generated automatically.\n";
		text += "# Pointers are RVAs from the main module base; scalars are\n";
		text += "# struct offsets / vtable slots. Delete this file (or pass\n";
		text += "# -cu3ml-noaddrcache) to force a full re-scan.\n";

		for (const auto &e : g_entries)
		{
			text += e.key;
			text += ' ';
			text += e.value;
			text += '\n';
		}

		if (write_file(g_path, text))
		{
			g_dirty = false;
			g_valid = true;
			log_info("addrcache: wrote %zu entries to %ls", g_entries.size(),
			         g_path.c_str());
		}
	}

	void invalidate(const char *why)
	{
		log_warn("addrcache: invalidated (%s)", why ? why : "unspecified");
		g_entries.clear();
		g_valid = false;
		g_dirty = true;
		prime_header(stamp_of(g_base));
	}

#define UE3_PTR_FIELDS(X)                               \
	X("ue3.GetPackageLinker", GetPackageLinker)         \
	X("ue3.StaticFindObjectFast", StaticFindObjectFast) \
	X("ue3.StaticLoadObject", StaticLoadObject)         \
	X("ue3.Preload", Preload)                           \
	X("ue3.FNameInit", FNameInit)                       \
	X("ue3.ArrayRealloc", ArrayRealloc)                 \
	X("ue3.GPackageFileCache", GPackageFileCache)       \
	X("ue3.GConfig", GConfig)                           \
	X("ue3.FNameNamesArr", FNameNamesArr)               \
	X("ue3.GSerializedObject", GSerializedObject)

#define UE3_VAL_FIELDS(X)                       \
	X("ue3.l_FArchiveOff", l_FArchiveOff)       \
	X("ue3.l_NameMap", l_NameMap)               \
	X("ue3.l_ImportMap", l_ImportMap)           \
	X("ue3.l_ExportMap", l_ExportMap)           \
	X("ue3.l_Loader", l_Loader)                 \
	X("ue3.l_OriginalLoader", l_OriginalLoader) \
	X("ue3.exp_stride", exp_stride)             \
	X("ue3.vt_Serialize", vt_Serialize)         \
	X("ue3.name.str_off", name.str_off)         \
	X("ue3.name.with_flags", name.with_flags)   \
	X("ue3.ar.Serialize", ar.Serialize)         \
	X("ue3.ar.SerializeName", ar.SerializeName) \
	X("ue3.ar.Tell", ar.Tell)                   \
	X("ue3.ar.Seek", ar.Seek)                   \
	X("ue3.ar.TotalSize", ar.TotalSize)         \
	X("ue3.ar.Precache", ar.Precache)           \
	X("ue3.ar.GetError", ar.GetError)           \
	X("ue3.ar.total", ar.total)                 \
	X("ue3.ar.validated", ar.validated)

	bool load_ue3(UE3Layout &out)
	{
		if (!g_valid)
			return false;

		UE3Layout t = out;
		const char *missing = nullptr;

#define LOAD_PTR(k, m)              \
	if (!missing)                   \
	{                               \
		void *p = nullptr;          \
		if (get_ptr(k, p))          \
			t.m = (decltype(t.m))p; \
		else                        \
			missing = k;            \
	}
		UE3_PTR_FIELDS(LOAD_PTR)
#undef LOAD_PTR

#define LOAD_VAL(k, m)              \
	if (!missing)                   \
	{                               \
		int64_t v = 0;              \
		if (get_i64(k, v))          \
			t.m = (decltype(t.m))v; \
		else                        \
			missing = k;            \
	}
		UE3_VAL_FIELDS(LOAD_VAL)
#undef LOAD_VAL

		if (missing)
		{
			log_warn("addrcache: entry '%s' missing — falling back to scan",
			         missing);
			return false;
		}

		t.ok = t.Preload && t.l_FArchiveOff && t.exp_stride && t.l_ExportMap &&
		       t.l_Loader && t.vt_Serialize && t.GSerializedObject &&
		       t.FNameNamesArr && t.name.str_off;

		if (!t.ok)
		{
			log_warn("addrcache: cached layout is unusable — re-scanning");
			return false;
		}

		out = t;
		return true;
	}

	void store_ue3(const UE3Layout &in)
	{
		if (!g_ready)
			return;

#define STORE_PTR(k, m) put_ptr(k, (const void *)in.m);
		UE3_PTR_FIELDS(STORE_PTR)
#undef STORE_PTR

#define STORE_VAL(k, m) put_i64(k, (int64_t)in.m);
		UE3_VAL_FIELDS(STORE_VAL)
#undef STORE_VAL
	}

#undef UE3_PTR_FIELDS
#undef UE3_VAL_FIELDS

#define LUA_PTR_FIELDS(X)                         \
	X("lua.Localize", Localize)                   \
	X("lua.LoadAllClasses", LoadAllClasses)       \
	X("lua.GetSectionPrivate", GetSectionPrivate) \
	X("lua.FindFile", FindFile)                   \
	X("lua.Combine", Combine)                     \
	X("lua.SectionAdd", SectionAdd)               \
	X("lua.SectionRemoveKey", SectionRemoveKey)   \
	X("lua.KeyCtor", KeyCtor)

#define LUA_VAL_FIELDS(X) X("lua.fname_keyed", fname_keyed)

	bool load_lua(LuaAddrLayout &out)
	{
		if (!g_valid)
			return false;

		LuaAddrLayout t = out;
		const char *missing = nullptr;

#define LOAD_PTR(k, m)              \
	if (!missing)                   \
	{                               \
		void *p = nullptr;          \
		if (get_ptr(k, p))          \
			t.m = (decltype(t.m))p; \
		else                        \
			missing = k;            \
	}
		LUA_PTR_FIELDS(LOAD_PTR)
#undef LOAD_PTR

#define LOAD_VAL(k, m)                     \
	if (!missing)                          \
	{                                      \
		int64_t v = 0;                     \
		if (get_i64(k, v))                 \
			t.m = (decltype(t.m))(v != 0); \
		else                               \
			missing = k;                   \
	}
		LUA_VAL_FIELDS(LOAD_VAL)
#undef LOAD_VAL

		if (missing)
		{
			log_warn("addrcache: entry '%s' missing - rescanning localize",
			         missing);
			return false;
		}

		t.ok = t.Localize && t.GetSectionPrivate && t.FindFile &&
		       t.SectionAdd && t.SectionRemoveKey &&
		       (!t.fname_keyed || t.KeyCtor);

		if (!t.ok)
		{
			log_warn("addrcache: cached localize layout unusable - rescanning");
			return false;
		}

		out = t;
		log_info("addrcache: localize layout restored from cache");
		return true;
	}

	void store_lua(const LuaAddrLayout &in)
	{
		if (!g_ready)
			return;

#define STORE_PTR(k, m) put_ptr(k, (const void *)in.m);
		LUA_PTR_FIELDS(STORE_PTR)
#undef STORE_PTR

#define STORE_VAL(k, m) put_i64(k, (int64_t)in.m);
		LUA_VAL_FIELDS(STORE_VAL)
#undef STORE_VAL
	}

#undef LUA_PTR_FIELDS
#undef LUA_VAL_FIELDS

}  // namespace addr_cache
