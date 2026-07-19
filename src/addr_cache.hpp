#pragma once
#define WIN32_LEAN_AND_MEAN
#include <cstdint>
#include <windows.h>

struct UE3Layout;

struct LuaAddrLayout
{
	void *Localize = nullptr;
	void *LoadAllClasses = nullptr;
	void *GetSectionPrivate = nullptr;
	void *FindFile = nullptr;
	void *Combine = nullptr;
	void *SectionAdd = nullptr;
	void *SectionRemoveKey = nullptr;
	void *KeyCtor = nullptr;
	bool fname_keyed = false;
	bool ok = false;
};

namespace addr_cache
{
	void init();

	bool loaded();

	bool dirty();

	bool get_ptr(const char *key, void *&out);
	void put_ptr(const char *key, const void *p);

	bool get_i64(const char *key, int64_t &out);
	void put_i64(const char *key, int64_t v);

	bool load_ue3(UE3Layout &out);

	void store_ue3(const UE3Layout &in);

	bool load_lua(LuaAddrLayout &out);

	void store_lua(const LuaAddrLayout &in);

	void save();

	void invalidate(const char *why);
}  // namespace addr_cache
