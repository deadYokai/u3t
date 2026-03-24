#include "mod_toml_mini.hpp"
#include "util.hpp"
#include <cctype>
#include <filesystem>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;
namespace mod_toml_mini {

namespace {

std::string trim(const std::string &s) {
	size_t a = s.find_first_not_of(" \t\r");
	size_t b = s.find_last_not_of(" \t\r");
	return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}

std::string strip_quotes(const std::string &s) {
	if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
	                      (s.front() == '\'' && s.back() == '\'')))
		return s.substr(1, s.size() - 2);
	return s;
}

std::pair<std::string, std::string> split_kv(const std::string &line) {
	auto eq = line.find('=');
	if (eq == std::string::npos)
		return {};
	return {trim(line.substr(0, eq)), trim(line.substr(eq + 1))};
}

std::string to_lower(std::string s) {
	for (auto &c : s)
		c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
	return s;
}

std::vector<std::string> parse_str_array(const std::string &val) {
	std::vector<std::string> out;
	auto lb = val.find('[');
	auto rb = val.rfind(']');
	if (lb == std::string::npos || rb == std::string::npos || lb >= rb)
		return out;

	std::string inner = val.substr(lb + 1, rb - lb - 1);
	std::istringstream ss(inner);
	std::string token;
	while (std::getline(ss, token, ',')) {
		token = trim(token);
		token = strip_quotes(token);
		if (!token.empty())
			out.push_back(token);
	}
	return out;
}

std::vector<int32_t> parse_int_array(const std::string &val) {
	std::vector<int32_t> out;
	auto lb = val.find('[');
	auto rb = val.rfind(']');
	if (lb == std::string::npos || rb == std::string::npos || lb >= rb)
		return out;

	std::string inner = val.substr(lb + 1, rb - lb - 1);
	std::istringstream ss(inner);
	std::string token;
	while (std::getline(ss, token, ',')) {
		token = trim(token);
		if (!token.empty()) {
			try {
				out.push_back(std::stoi(token));
			} catch (...) {
			}
		}
	}
	return out;
}

} // namespace

ParseResult parse(const std::string &text, const std::string &mod_dir_str) {
	ParseResult result;
	fs::path mod_dir(mod_dir_str);

	int16_t upk_version = 684;

	enum class Section { None, UPK, Replace } sec = Section::None;

	std::string cur_stem;
	std::vector<std::string> cur_blobs;
	std::vector<int32_t> cur_outers;

	std::string cur_orig;
	std::string cur_repl;

	std::unordered_map<std::string, size_t> stem_map;
	auto flush_upk = [&]() {
		if (cur_stem.empty() || cur_blobs.empty()) {
			cur_stem.clear();
			cur_blobs.clear();
			cur_outers.clear();
			return;
		}
		UPKBlobManifest m;
		m.stem = cur_stem;
		m.mod_dir = mod_dir.wstring();
		m.blobs = cur_blobs;
		m.outers = cur_outers;
		m.ver = upk_version;
		stem_map[to_lower(cur_stem)] = result.manifests.size();
		result.manifests.push_back(std::move(m));
		cur_stem.clear();
		cur_blobs.clear();
		cur_outers.clear();
	};

	auto flush_replace = [&]() {
		if (cur_orig.empty() || cur_repl.empty()) {
			cur_orig.clear();
			cur_repl.clear();
			return;
		}

		auto orig_dot = cur_orig.rfind('.');
		std::string obj_name = (orig_dot != std::string::npos)
		                           ? cur_orig.substr(orig_dot + 1)
		                           : cur_orig;

		std::string repl_full = cur_repl;
		if (repl_full.find('.') == std::string::npos)
			repl_full = cur_repl + "." + cur_repl; // "MyFont" → "MyFont.MyFont"

		auto pkg_dot = repl_full.rfind('.');
		std::string repl_pkg = repl_full.substr(0, pkg_dot);
		std::string repl_pkg_lo = to_lower(repl_pkg);
		std::string repl_obj = repl_full.substr(pkg_dot + 1);
		std::string repl_obj_lo = to_lower(repl_obj);

		ObjectReplacement r;
		r.orig_obj = obj_name;
		r.orig_full_path = cur_orig;
		r.repl_path_w = to_wide(repl_full);

		auto mit = stem_map.find(repl_pkg_lo);
		if (mit != stem_map.end()) {
			const UPKBlobManifest &m = result.manifests[mit->second];

			std::string ue3_class;
			for (auto &blob : m.blobs) {
				fs::path bp(blob);
				if (to_lower(bp.stem().string()) == repl_obj_lo) {
					std::string ext = bp.extension().string();
					if (ext.size() > 1 && ext[0] == '.')
						ue3_class = ext.substr(1);
					break;
				}
			}
			if (ue3_class.empty() && !m.blobs.empty()) {
				fs::path bp(m.blobs[0]);
				std::string ext = bp.extension().string();
				if (ext.size() > 1 && ext[0] == '.')
					ue3_class = ext.substr(1);
			}

			r.ue3_class = ue3_class;
			r.is_blob = true;
			r.blob_stem = repl_pkg;

		} else {
			std::wstring file_path;
			std::string ue3_class;

			std::wstring pkg_fallback_path;
			std::string pkg_fallback_class;

			try {
				for (const auto &e : fs::directory_iterator(mod_dir)) {
					if (!e.is_regular_file())
						continue;
					std::string stem = to_lower(e.path().stem().string());
					std::string ext = e.path().extension().string();

					if (stem == repl_obj_lo) {
						file_path = e.path().wstring();
						ue3_class = (ext.size() > 1 && ext[0] == '.')
						                ? ext.substr(1)
						                : "";
						pkg_fallback_path.clear();
						break;
					}
					if (stem == repl_pkg_lo && pkg_fallback_path.empty()) {
						pkg_fallback_path = e.path().wstring();
						pkg_fallback_class = (ext.size() > 1 && ext[0] == '.')
						                         ? ext.substr(1)
						                         : "";
					}
				}
			} catch (...) {
			}

			if (file_path.empty()) {
				file_path = pkg_fallback_path;
				ue3_class = pkg_fallback_class;
			}

			r.ue3_class = ue3_class;
			r.mod_upk_w = file_path;
		}

		result.replacements.push_back(std::move(r));
		cur_orig.clear();
		cur_repl.clear();
	};

	std::istringstream ss(text);
	std::string raw;
	while (std::getline(ss, raw)) {
		std::string line = trim(raw);
		auto hash = line.find('#');
		if (hash != std::string::npos)
			line = trim(line.substr(0, hash));
		if (line.empty())
			continue;

		if (line == "[[patch.upk]]") {
			flush_upk();
			sec = Section::UPK;
			continue;
		}
		if (line == "[[patch.replace]]") {
			flush_upk();
			flush_replace();
			sec = Section::Replace;
			continue;
		}
		if (line == "[[patch]]" || line == "[patch.name_remap]") {
			flush_upk();
			flush_replace();
			sec = Section::None;
			continue;
		}

		auto [k, raw_v] = split_kv(line);
		if (k.empty())
			continue;
		std::string v = strip_quotes(raw_v);

		if (sec == Section::None) {
			if (k == "upk_version") {
				try {
					upk_version = (int16_t)std::stoi(v);
				} catch (...) {
				}
			}
		} else if (sec == Section::UPK) {
			if (k == "stem")
				cur_stem = v;
			else if (k == "blobs")
				cur_blobs = parse_str_array(raw_v);
			else if (k == "outers")
				cur_outers = parse_int_array(raw_v);
		} else if (sec == Section::Replace) {
			if (k == "original")
				cur_orig = v;
			else if (k == "replacement")
				cur_repl = v;
		}
	}
	flush_upk();
	flush_replace();
	return result;
}

} // namespace mod_toml_mini
