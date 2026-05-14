#pragma once
#include "config.hpp"
#include <string>
#include <vector>

namespace override_loader
{
	struct ImpEntry
	{
		int32_t mod_index;
		std::string path;
	};

	struct OverrideRecord
	{
		std::wstring key;
		std::vector<uint8_t> bin;
		std::vector<std::pair<int32_t, void *>> resolved_imports;
		bool imports_resolved;
		std::vector<ImpEntry> imp_entries;
		int32_t upk_version = 684;
	};

	void discover(const std::vector<LoadedMod> &mods);
	void install_hooks();
	OverrideRecord *find(const std::wstring &key);
	size_t count();
}  // namespace override_loader
