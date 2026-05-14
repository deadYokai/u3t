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

}  // namespace mod_loader
