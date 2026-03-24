#pragma once
#include "mod_loader.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace mod_toml_mini {

struct ParseResult {
	std::vector<ObjectReplacement> replacements;
	std::vector<UPKBlobManifest> manifests;
};

ParseResult parse(const std::string &text, const std::string &mod_dir_str);

} // namespace mod_toml_mini
