#pragma once
#include "ue3_types.hpp"
#include <cstdint>
#include <string>
#include <vector>

struct UPKBlobManifest {
	std::string stem;
	std::wstring mod_dir;
	std::vector<std::string> blobs;
	std::vector<int32_t> outers;
	int16_t ver{684};
};

struct ObjectReplacement {
	std::string orig_obj;
	std::string orig_full_path;
	std::wstring repl_path_w;
	std::string ue3_class;

	bool is_blob{};
	std::string blob_stem;

	std::wstring mod_upk_w;

	FName orig_name{};
	void *cached_cls{};
	void *cached_obj{};

	bool cls_resolved{};
	bool orig_found{};
	bool orig_warned{};
	bool slo_failed{};
};

namespace mod_loader {
void ensure_loaded();
void *find_replacement(FName name, void *outer, void *hook_cls);
std::wstring find_mod_pkg_path(const wchar_t *pkg_name);
} // namespace mod_loader
