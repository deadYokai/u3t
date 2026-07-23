#include "lua_lib.hpp"
#include <lua.hpp>

static int l_hello(lua_State *L)
{
	lua_pushstring(L, "from native code");
	return 1;
}

extern "C" __declspec(dllexport) int
CU3MLPluginInit(lua_State *L, const CU3MLHostApi *api)
{
	if (!api || api->abi != CU3ML_PLUGIN_ABI)
		return -1;
	api->log_info("MyPlugin starting");

	lua_newtable(L);
	lua_pushcfunction(L, l_hello);
	lua_setfield(L, -2, "hello");
	lua_setglobal(L, "myplugin");
	return 0;
}
