#pragma once
#include "config.hpp"
#include <string>
#include <vector>

namespace mod_loader
{
	void discover();
	void register_content();
	void install_hooks();
	bool find_replace(const std::wstring &orig, std::wstring &out);
	const std::vector<LoadedMod> &loaded_mods();
	const std::vector<LoadedMod> &enabled_mods();
	void set_enabled(size_t index, bool on);
	void refresh();

}  // namespace mod_loader
