#define WIN32_LEAN_AND_MEAN
#include "mod_loader.hpp"
#include "config.hpp"
#include "hook.hpp"
#include "loader_config.hpp"
#include "logs.hpp"
#include "ue3_layout.hpp"
#include "util.hpp"
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>

using StaticLoadObjectFn = void *(__cdecl *)(void *, void *, const wchar_t *,
                                             const wchar_t *, uint32_t, void *,
                                             int32_t);

using CachePathFn = void(__cdecl *)(void *, const wchar_t *);

namespace
{
	struct ReplaceEntry
	{
		std::wstring original;
		std::wstring replacement;
	};

	std::vector<ReplaceEntry> g_replaces;
	std::vector<LoadedMod> g_mods;
	std::vector<LoadedMod> g_active;

	static StaticLoadObjectFn g_orig_slo = nullptr;
	static thread_local bool s_in_hook = false;

	static void *__cdecl
	hook_StaticLoadObject(void *InClass, void *InOuter, const wchar_t *Name,
	                      const wchar_t *Filename, uint32_t LoadFlags,
	                      void *Sandbox, int32_t bAllowReconciliation)
	{
		if (!Name || s_in_hook)
			return g_orig_slo(InClass, InOuter, Name, Filename, LoadFlags,
			                  Sandbox, bAllowReconciliation);

		for (const auto &r : g_replaces)
		{
			if (r.original == Name)
			{
				log_info("slo_hook: replace '%ls' -> '%ls'", Name,
				         r.replacement.c_str());
				s_in_hook = true;
				void *res =
				    g_orig_slo(InClass, nullptr, r.replacement.c_str(), nullptr,
				               LoadFlags, Sandbox, bAllowReconciliation);
				s_in_hook = false;
				return res;
			}
		}

		return g_orig_slo(InClass, InOuter, Name, Filename, LoadFlags, Sandbox,
		                  bAllowReconciliation);
	}

	static std::vector<LoadedMod> topo_sort(std::vector<LoadedMod> mods)
	{
		const size_t n = mods.size();
		if (n <= 1)
			return mods;

		std::unordered_map<std::string, size_t> idx;
		for (size_t i = 0; i < n; ++i)
			idx[mods[i].cfg.name] = i;

		std::vector<std::vector<size_t>> dep_of(n);
		std::vector<size_t> in_degree(n, 0);

		for (size_t i = 0; i < n; ++i)
		{
			for (const auto &req : mods[i].cfg.dependencies)
			{
				auto it = idx.find(req);
				if (it == idx.end())
				{
					log_warn("mod_loader: '%s' requires unknown mod '%s'",
					         mods[i].cfg.name.c_str(), req.c_str());
					continue;
				}
				dep_of[it->second].push_back(i);
				++in_degree[i];
			}
		}

		std::vector<size_t> queue, sorted;
		for (size_t i = 0; i < n; ++i)
			if (in_degree[i] == 0)
				queue.push_back(i);

		while (!queue.empty())
		{
			size_t cur = queue.front();
			queue.erase(queue.begin());
			sorted.push_back(cur);
			for (size_t nxt : dep_of[cur])
				if (--in_degree[nxt] == 0)
					queue.push_back(nxt);
		}

		if (sorted.size() != n)
		{
			log_warn("mod_loader: dependency cycle detected — affected mods "
			         "appended in original order");
			for (size_t i = 0; i < n; ++i)
				if (in_degree[i] > 0)
				{
					log_warn("mod_loader: cycle involves '%s'",
					         mods[i].cfg.name.c_str());
					sorted.push_back(i);
				}
		}

		std::vector<LoadedMod> out;
		out.reserve(n);
		for (size_t i : sorted)
			out.push_back(std::move(mods[i]));
		return out;
	}

	static void discover_mods()
	{
		std::wstring mods_dir = get_mods_dir();
		CreateDirectoryW(mods_dir.c_str(), nullptr);

		WIN32_FIND_DATAW fd{};
		HANDLE h = FindFirstFileW((mods_dir + L"\\*").c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE)
			return;

		do
		{
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				continue;
			if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
				continue;

			std::wstring dir_w = mods_dir + L"\\" + fd.cFileName;
			std::wstring toml_w = dir_w + L"\\mod.toml";
			if (GetFileAttributesW(toml_w.c_str()) == INVALID_FILE_ATTRIBUTES)
				continue;

			LoadedMod lm;
			lm.dir_w = dir_w;
			lm.key = to_narrow(fd.cFileName);
			if (!parse_mod_config(to_narrow(toml_w), lm.cfg))
			{
				log_warn("mod_loader: skipping '%ls' (parse error)",
				         fd.cFileName);
				continue;
			}
			if (lm.cfg.name.empty())
				lm.cfg.name = lm.key;

			lm.cfg.enabled = loader_config::mod_enabled(lm.key, lm.cfg.enabled);
			loader_config::set_mod_enabled(lm.key, lm.cfg.enabled);

			g_mods.push_back(std::move(lm));
			log_info("mod_loader: found '%s'  v%s  by %s  deps=[%zu]  %s",
			         g_mods.back().cfg.name.c_str(),
			         g_mods.back().cfg.version.empty()
			             ? "?"
			             : g_mods.back().cfg.version.c_str(),
			         g_mods.back().cfg.author.empty()
			             ? "unknown"
			             : g_mods.back().cfg.author.c_str(),
			         g_mods.back().cfg.dependencies.size(),
			         g_mods.back().cfg.enabled ? "enabled" : "DISABLED");
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}

	static void build_tables()
	{
		g_replaces.clear();
		g_active.clear();

		for (const auto &lm : g_mods)
		{
			if (!lm.cfg.enabled)
				continue;

			g_active.push_back(lm);

			for (const auto &rp : lm.cfg.replace_patches)
			{
				if (rp.original.empty() || rp.replacement.empty())
				{
					log_warn("mod_loader: [[patch.replace]] in '%s' incomplete",
					         lm.cfg.name.c_str());
					continue;
				}
				g_replaces.push_back(
				    {to_wide(rp.original), to_wide(rp.replacement)});
				log_info("mod_loader: replace  '%s'  ->  '%s'",
				         rp.original.c_str(), rp.replacement.c_str());
			}
		}
	}

	static constexpr int kVtSlot_CachePath = 4;

	static void cache_content_path(void *cache_obj, const std::wstring &path)
	{
		if (!cache_obj)
			return;
		void **vt = *reinterpret_cast<void ***>(cache_obj);
		reinterpret_cast<CachePathFn>(vt[kVtSlot_CachePath])(cache_obj,
		                                                     path.c_str());
		log_info("register_content: CachePath('%ls')", path.c_str());
	}
}  // namespace

namespace mod_loader
{
	void discover()
	{
		discover_mods();
		g_mods = topo_sort(std::move(g_mods));

		std::vector<std::string> keys;
		keys.reserve(g_mods.size());
		for (const auto &lm : g_mods)
			keys.push_back(lm.key);
		loader_config::retain_mods(keys);
		loader_config::save_if_dirty();

		build_tables();
		log_info("mod_loader: discovery done — %zu mod(s), %zu enabled, "
		         "%zu replace(s)",
		         g_mods.size(), g_active.size(), g_replaces.size());
	}

	void refresh()
	{
		build_tables();
		log_info("mod_loader: refreshed — %zu of %zu mod(s) enabled, "
		         "%zu replace(s)",
		         g_active.size(), g_mods.size(), g_replaces.size());
	}

	void set_enabled(size_t index, bool on)
	{
		if (index >= g_mods.size())
			return;
		if (g_mods[index].cfg.enabled == on)
			return;

		g_mods[index].cfg.enabled = on;
		loader_config::set_mod_enabled(g_mods[index].key, on);
		log_info("mod_loader: '%s' %s", g_mods[index].cfg.name.c_str(),
		         on ? "enabled" : "disabled");
	}

	void register_content()
	{
		if (!ue3().GPackageFileCache)
		{
			log_warn("register_content: GPackageFileCache not resolved — "
			         "content mods will not be registered");
			return;
		}
		void *cache = *ue3().GPackageFileCache;
		if (!cache)
		{
			log_warn(
			    "register_content: *GPackageFileCache is null — engine may "
			    "not have initialised it yet");
			return;
		}

		for (const auto &lm : g_active)
			for (const auto &rel : lm.cfg.content_paths)
				cache_content_path(cache, lm.dir_w + L"\\" + to_wide(rel));
	}

	void install_hooks()
	{
		if (!ue3().StaticLoadObject)
		{
			log_warn(
			    "mod_loader: StaticLoadObject not resolved — hook skipped");
			return;
		}
		if (g_replaces.empty())
		{
			log_info("mod_loader: no replace patches — SLO hook skipped");
			return;
		}
		hook::add(ue3().StaticLoadObject,
		          reinterpret_cast<void *>(&hook_StaticLoadObject),
		          reinterpret_cast<void **>(&g_orig_slo));
		log_info("mod_loader: SLO hook queued  (%zu replaces)",
		         g_replaces.size());
	}

	bool find_replace(const std::wstring &orig, std::wstring &out)
	{
		for (const auto &r : g_replaces)
			if (r.original == orig)
			{
				out = r.replacement;
				return true;
			}
		return false;
	}

	const std::vector<LoadedMod> &loaded_mods() { return g_mods; }

	const std::vector<LoadedMod> &enabled_mods() { return g_active; }
}  // namespace mod_loader
