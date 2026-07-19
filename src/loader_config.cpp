#define WIN32_LEAN_AND_MEAN
#include "loader_config.hpp"
#include "logs.hpp"
#include "util.hpp"

#include <fstream>
#include <windows.h>

namespace
{
	LoaderSettings g_settings;

	std::vector<std::pair<std::string, bool>> g_mods;
	bool g_dirty = false;

	std::string trim(const std::string &s)
	{
		size_t a = s.find_first_not_of(" \t\r\n");
		if (a == std::string::npos)
			return {};
		size_t b = s.find_last_not_of(" \t\r\n");
		return s.substr(a, b - a + 1);
	}

	std::string strip_comment(const std::string &l)
	{
		bool in = false;
		for (size_t i = 0; i < l.size(); ++i)
		{
			if (l[i] == '"')
				in = !in;
			if (!in && l[i] == '#')
				return l.substr(0, i);
		}
		return l;
	}

	std::string unquote(const std::string &v)
	{
		if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
			return v.substr(1, v.size() - 2);
		return v;
	}

	std::string quote(const std::string &v)
	{
		std::string out = "\"";
		for (char c : v)
		{
			if (c == '"' || c == '\\')
				out += '\\';
			out += c;
		}
		out += '"';
		return out;
	}

	bool parse_bool(const std::string &v, bool fallback)
	{
		std::string s = v;
		for (auto &c : s)
			c = (char)tolower((unsigned char)c);
		if (s == "true" || s == "1" || s == "yes" || s == "on")
			return true;
		if (s == "false" || s == "0" || s == "no" || s == "off")
			return false;
		return fallback;
	}

	bool iequals(const std::string &a, const std::string &b)
	{
		if (a.size() != b.size())
			return false;
		for (size_t i = 0; i < a.size(); ++i)
			if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
				return false;
		return true;
	}

	std::pair<std::string, bool> *find_mod(const std::string &key)
	{
		for (auto &e : g_mods)
			if (iequals(e.first, key))
				return &e;
		return nullptr;
	}
}  // namespace

namespace loader_config
{
	std::wstring path()
	{
		std::wstring dir = get_mods_dir();
		if (dir.empty())
			return {};
		return dir + L"\\cu3ml.toml";
	}

	LoaderSettings &settings() { return g_settings; }

	void mark_dirty() { g_dirty = true; }

	bool load()
	{
		std::wstring p = path();
		if (p.empty())
		{
			log_warn("loader_config: mods dir unresolved — using defaults");
			return false;
		}

		std::ifstream f(to_narrow(p));
		if (!f)
		{
			log_info("loader_config: %ls not found — using defaults", p.c_str());
			g_dirty = true;
			return false;
		}

		g_mods.clear();
		g_settings = LoaderSettings{};

		std::string table, line;
		while (std::getline(f, line))
		{
			line = trim(strip_comment(line));
			if (line.empty())
				continue;

			if (line.front() == '[')
			{
				size_t e = line.find(']');
				if (e == std::string::npos)
					continue;
				table = trim(line.substr(1, e - 1));
				continue;
			}

			size_t eq = line.find('=');
			if (eq == std::string::npos)
				continue;
			std::string k = unquote(trim(line.substr(0, eq)));
			std::string v = trim(line.substr(eq + 1));
			if (k.empty())
				continue;

			if (table == "loader")
			{
				if (k == "lua")
					g_settings.lua = parse_bool(v, g_settings.lua);
				else if (k == "lua_verbose")
					g_settings.lua_verbose =
					    parse_bool(v, g_settings.lua_verbose);
				else if (k == "manager_at_start")
					g_settings.manager_at_start =
					    parse_bool(v, g_settings.manager_at_start);
				else
					log_warn("loader_config: unknown [loader] key '%s'",
					         k.c_str());
			}
			else if (table == "mods")
			{
				if (!find_mod(k))
					g_mods.push_back({k, parse_bool(v, true)});
			}
		}

		g_dirty = false;
		log_info("loader_config: loaded %ls — lua=%d lua_verbose=%d "
		         "manager_at_start=%d mods=%zu",
		         p.c_str(), (int)g_settings.lua, (int)g_settings.lua_verbose,
		         (int)g_settings.manager_at_start, g_mods.size());
		return true;
	}

	bool save()
	{
		std::wstring dir = get_mods_dir();
		std::wstring p = path();
		if (p.empty())
		{
			log_err("loader_config: cannot save — mods dir unresolved");
			return false;
		}
		CreateDirectoryW(dir.c_str(), nullptr);

		std::ofstream f(to_narrow(p), std::ios::binary | std::ios::trunc);
		if (!f)
		{
			log_err("loader_config: cannot write %ls", p.c_str());
			return false;
		}

		auto b = [](bool v) { return v ? "true" : "false"; };

		f << "# CU3ML - crappy unreal engine 3 mod loader\n";
		f << "# Written by the loader. Safe to edit while the game is "
		     "closed.\n\n";

		f << "[loader]\n";
		f << "# embedded Lua VM and mod scripts (-nolua overrides this)\n";
		f << "lua = " << b(g_settings.lua) << "\n";
		f << "# verbose Lua logging (-luaverbose overrides this)\n";
		f << "lua_verbose = " << b(g_settings.lua_verbose) << "\n";
		f << "# open the manager on every launch, without -cu3ml-manager\n";
		f << "manager_at_start = " << b(g_settings.manager_at_start) << "\n\n";

		f << "[mods]\n";
		f << "# key = mod folder name inside this directory\n";
		for (const auto &e : g_mods)
			f << quote(e.first) << " = " << b(e.second) << "\n";

		if (!f.good())
		{
			log_err("loader_config: write failed for %ls", p.c_str());
			return false;
		}

		g_dirty = false;
		log_info("loader_config: saved %ls (%zu mod entries)", p.c_str(),
		         g_mods.size());
		return true;
	}

	bool save_if_dirty() { return g_dirty ? save() : true; }

	bool has_mod(const std::string &key) { return find_mod(key) != nullptr; }

	bool mod_enabled(const std::string &key, bool fallback)
	{
		auto *e = find_mod(key);
		return e ? e->second : fallback;
	}

	void set_mod_enabled(const std::string &key, bool on)
	{
		if (auto *e = find_mod(key))
		{
			if (e->second != on)
			{
				e->second = on;
				g_dirty = true;
			}
			return;
		}
		g_mods.push_back({key, on});
		g_dirty = true;
	}

	void retain_mods(const std::vector<std::string> &keys)
	{
		std::vector<std::pair<std::string, bool>> out;
		out.reserve(keys.size());
		for (const auto &k : keys)
		{
			auto *e = find_mod(k);
			out.push_back({k, e ? e->second : true});
		}

		if (out.size() != g_mods.size())
			g_dirty = true;
		else
			for (size_t i = 0; i < out.size(); ++i)
				if (out[i] != g_mods[i])
				{
					g_dirty = true;
					break;
				}

		g_mods.swap(out);
	}
}  // namespace loader_config
