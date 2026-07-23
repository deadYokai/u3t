#pragma once
#define WIN32_LEAN_AND_MEAN

#include <cstdint>

#include <windows.h>

struct lua_State;

extern "C"
{
	struct CU3MLHostApi
	{
		uint32_t size;
		uint32_t abi;

		void (*log_info)(const char *msg);
		void (*log_warn)(const char *msg);
		void (*log_err)(const char *msg);

		void *StaticLoadObject;
		void *StaticFindObjectFast;
		void *FNameInit;
		void *ArrayRealloc;
		void *module_base;
	};

	typedef int(__cdecl *CU3MLPluginInitFn)(lua_State *L,
	                                        const CU3MLHostApi *api);
}

#define CU3ML_PLUGIN_ABI 1u

namespace sol
{
	class state;
}

namespace lua_lib
{
	void bind(sol::state &lua);

	void unload_all();
}  // namespace lua_lib
