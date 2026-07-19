#pragma once
#include <string>
#include <vector>

struct LoaderSettings
{
	bool lua = true;
	bool lua_verbose = false;
	bool manager_at_start = false;
};

namespace loader_config
{
	std::wstring path();

	LoaderSettings &settings();

	bool load();

	bool save();

	bool save_if_dirty();

	void mark_dirty();

	bool has_mod(const std::string &key);
	bool mod_enabled(const std::string &key, bool fallback);
	void set_mod_enabled(const std::string &key, bool on);

	void retain_mods(const std::vector<std::string> &keys);
}  // namespace loader_config
