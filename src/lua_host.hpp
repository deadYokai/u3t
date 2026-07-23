#pragma once

#define WIN32_LEAN_AND_MEAN
#include <string>
#include <windows.h>

namespace lua_host
{
	void configure_from_cmdline();

	void init();

	void shutdown();

	bool enabled();
	bool initialized();

	bool run_string(const std::string &code,
	                const char *chunk_name = "=console");

	bool run_file(const std::wstring &path);

	void notify_preloaded(void *obj);

}  // namespace lua_host
