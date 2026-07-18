#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct SpawnPatch
{
	std::string class_name;
	std::string package_name;
	std::string object_name;
};

struct ReplacePatch
{
	std::string original;
	std::string replacement;
};

struct ModConfig
{
	std::string mod_dir;
	std::string name;
	std::string author;
	std::string version;
	std::string description;
	bool enabled = true;

	std::vector<std::string> content_path;

	std::vector<std::string> dependencies;

	std::vector<std::string> content_paths;
	std::vector<SpawnPatch> spawn_patches;
	std::vector<ReplacePatch> replace_patches;
};

struct LoadedMod
{
	ModConfig cfg;
	std::wstring dir_w;
};

bool parse_mod_config(const std::string &toml_path, ModConfig &out);
