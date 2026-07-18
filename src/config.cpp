#define WIN32_LEAN_AND_MEAN
#include "config.hpp"
#include "logs.hpp"
#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>

static std::string trim(const std::string &s)
{
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos)
		return {};
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

static std::string strip_comment(const std::string &l)
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

static std::string parse_str(const std::string &v)
{
	if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
	{
		std::string s = v.substr(1, v.size() - 2), r;
		for (size_t i = 0; i < s.size(); ++i)
		{
			if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '"')
			{
				r += '"';
				++i;
			}
			else
				r += s[i];
		}
		return r;
	}
	return v;
}

static uint32_t parse_u32(const std::string &s)
{
	try
	{
		return (uint32_t)std::stoul(s);
	}
	catch (...)
	{
		return 0;
	}
}

static std::vector<std::string> parse_str_array(const std::string &v)
{
	std::vector<std::string> out;
	if (v.empty() || v.front() != '[')
		return out;
	size_t i = 0;
	while (i < v.size() && v[i] != ']')
	{
		while (i < v.size() && (v[i] == ' ' || v[i] == '\t' || v[i] == ','))
			++i;

		if (i >= v.size() || v[i] == ']')
			break;

		if (v[i] != '"')
		{
			size_t end = v.find_first_of(",]", i);
			out.push_back(trim(
			    v.substr(i, (end == std::string::npos ? v.size() : end) - i)));
			i = (end == std::string::npos) ? v.size() : end;
		}
		else
		{
			size_t close = v.find('"', i + 1);
			if (close == std::string::npos)
				break;

			out.push_back(v.substr(i + 1, close - i - 1));
			i = close + 1;
		}
	}
	return out;
}

bool parse_mod_config(const std::string &path, ModConfig &out)
{
	std::ifstream f(path);
	if (!f)
	{
		log_err("config: cannot open %s", path.c_str());
		return false;
	}

	{
		auto p = path.find_last_of("/\\");
		out.mod_dir = (p != std::string::npos) ? path.substr(0, p) : ".";
	}

	std::string cur_table;
	SpawnPatch *cur_sp = nullptr;
	ReplacePatch *cur_rp = nullptr;

	auto flush_sp = [&]()
	{
		if (cur_sp && (cur_sp->class_name.empty()))
			log_warn("config: incomplete [[patch.spawn]] in %s", path.c_str());
	};

	std::string line;
	while (std::getline(f, line))
	{
		line = trim(strip_comment(line));
		if (line.empty())
			continue;

		if (line.rfind("[[", 0) == 0)
		{
			flush_sp();
			auto e = line.find("]]");
			if (e == std::string::npos)
				continue;
			cur_table = trim(line.substr(2, e - 2));
			cur_sp = nullptr;
			cur_rp = nullptr;

			if (cur_table == "patch.spawn")
			{
				out.spawn_patches.push_back({});
				cur_sp = &out.spawn_patches.back();
			}
			else if (cur_table == "patch.replace")
			{
				out.replace_patches.push_back({});
				cur_rp = &out.replace_patches.back();
			}
			continue;
		}
		if (line.rfind("[", 0) == 0 && line.rfind("[[", 0) != 0)
		{
			flush_sp();
			cur_table.clear();
			cur_sp = nullptr;
			cur_rp = nullptr;
			continue;
		}

		auto eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		std::string k = trim(line.substr(0, eq));
		std::string v = trim(line.substr(eq + 1));

		if (cur_sp)
		{
			if (k == "class")
				cur_sp->class_name = parse_str(v);
			else if (k == "package")
				cur_sp->package_name = parse_str(v);
			else if (k == "name")
				cur_sp->object_name = parse_str(v);
		}
		else if (cur_rp)
		{
			if (k == "original")
				cur_rp->original = parse_str(v);
			else if (k == "replacement")
				cur_rp->replacement = parse_str(v);
		}
		else if (cur_table == "dependencies")
		{
			if (k == "requires")
				out.dependencies = parse_str_array(v);
		}
		else
		{
			if (k == "name")
				out.name = parse_str(v);
			else if (k == "author")
				out.author = parse_str(v);
			else if (k == "version")
				out.version = parse_str(v);
			else if (k == "description")
				out.description = parse_str(v);
			else if (k == "content_path")
				out.content_paths.push_back(parse_str(v));
			else if (k == "enabled")
				out.enabled = (trim(v) != "false");
		}
	}
	flush_sp();

	return true;
}
