#pragma once
#include "config.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace override_loader
{
	struct OverrideRecord
	{
		std::wstring pkg;
		std::wstring key;
		std::vector<uint8_t> bin;
		std::vector<std::string> tool_names;
		std::vector<int32_t> name_remap;
		bool name_remap_ready = false;
		std::vector<int32_t> name_map_final;
	};

	void discover(const std::vector<LoadedMod> &mods);
	void install_hooks();
	OverrideRecord *find(const std::wstring &key);
	size_t count();
}  // namespace override_loader
