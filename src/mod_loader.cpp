#define WIN32_LEAN_AND_MEAN
#include "mod_loader.hpp"
#include "config.hpp"
#include "hook.hpp"
#include "logs.hpp"
#include "ue3_types.hpp"
#include "util.hpp"
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>

using StaticLoadObjectFn = void *(__cdecl *)(void *, void *, const wchar_t *,
                                             const wchar_t *, uint32_t, void *,
                                             int32_t);

using StaticConstructObjectFn = void *(__cdecl *)(void *, void *, FNameOnStack,
                                                  uint64_t, void *, void *,
                                                  void *, void *);

using CreatePackageFn = void *(__cdecl *)(void *, const wchar_t *, uint32_t);

using FNameInitFn = void(__cdecl *)(void *, const wchar_t *, int32_t);

using CachePathFn = void(__cdecl *)(void *, const wchar_t *);

static constexpr uint64_t kRF_Public = 0x0000000400000000ULL;
static constexpr uint64_t kRF_Standalone = 0x0008000000000000ULL;

static constexpr int32_t kFNAME_Add = 1;

namespace
{

	struct SpawnEntry
	{
		std::wstring key;
		std::string class_name;
		std::wstring package_name;
		std::wstring object_name;

		void *spawned = nullptr;
	};

	struct ReplaceEntry
	{
		std::wstring original;
		std::wstring replacement;
	};

	std::vector<SpawnEntry> g_spawns;
	std::vector<ReplaceEntry> g_replaces;

	std::vector<LoadedMod> g_mods;

	static StaticLoadObjectFn g_orig_slo = nullptr;

	static thread_local bool s_in_spawn = false;

	static FNameOnStack make_fname(const wchar_t *name)
	{
		FNameOnStack fn{};
		if (ue3().FNameInit)
			reinterpret_cast<FNameInitFn>(ue3().FNameInit)(&fn, name,
			                                               kFNAME_Add);
		return fn;
	}

	static void *find_class(const wchar_t *class_name)
	{
		if (!ue3().StaticFindObjectFast)
			return nullptr;

		using SFOFFn = void *(__cdecl *)(void *, void *, FNameOnStack, int32_t,
		                                 int32_t, uint64_t);
		FNameOnStack fn = make_fname(class_name);
		return reinterpret_cast<SFOFFn>(ue3().StaticFindObjectFast)(
		    nullptr, nullptr, fn, 0, 1, 0);
	}

	static void *spawn_object(SpawnEntry &e)
	{

		void *obj = static_cast<void *>(
		    InterlockedCompareExchangePointer(&e.spawned, nullptr, nullptr));
		if (obj)
			return obj;

		if (!ue3().CreatePackage || !ue3().StaticConstructObject)
		{
			log_err("spawn: CreatePackage or StaticConstructObject not resolved"
			        " — cannot spawn '%ls'",
			        e.key.c_str());
			return nullptr;
		}

		std::wstring wcls(e.class_name.begin(), e.class_name.end());
		void *cls = find_class(wcls.c_str());
		if (!cls)
		{
			log_err(
			    "spawn: class '%s' not found in memory — is the package that"
			    " defines it loaded yet?",
			    e.class_name.c_str());
			return nullptr;
		}

		void *pkg = reinterpret_cast<CreatePackageFn>(ue3().CreatePackage)(
		    nullptr, e.package_name.c_str(), 0);
		if (!pkg)
		{
			log_err("spawn: CreatePackage failed for '%ls'",
			        e.package_name.c_str());
			return nullptr;
		}

		FNameOnStack obj_name = make_fname(e.object_name.c_str());
		void *new_obj = reinterpret_cast<StaticConstructObjectFn>(
		    ue3().StaticConstructObject)(cls, pkg, obj_name,
		                                 kRF_Public | kRF_Standalone, nullptr,
		                                 nullptr, nullptr, nullptr);

		if (!new_obj)
		{
			log_err("spawn: StaticConstructObject failed for '%ls'",
			        e.key.c_str());
			return nullptr;
		}

		void *prev =
		    InterlockedCompareExchangePointer(&e.spawned, new_obj, nullptr);
		if (prev)
		{
			log_info("spawn: race on '%ls' — using peer's object %p",
			         e.key.c_str(), prev);
			return prev;
		}

		log_info("spawn: created %s.%ls  class=%s  obj=%p",
		         e.class_name.c_str(), e.object_name.c_str(),
		         e.class_name.c_str(), new_obj);
		return new_obj;
	}

	static std::wstring base_key(const wchar_t *name)
	{
		const wchar_t *last_dot = wcsrchr(name, L'.');
		if (!last_dot)
			return {};

		std::wstring full(name);
		size_t second = full.rfind(L'.', last_dot - name - 1);
		if (second == std::wstring::npos)
			return full;
		return full.substr(second + 1);
	}

	static void *__cdecl
	hook_StaticLoadObject(void *InClass, void *InOuter, const wchar_t *Name,
	                      const wchar_t *Filename, uint32_t LoadFlags,
	                      void *Sandbox, int32_t bAllowReconciliation)
	{
		if (!Name || s_in_spawn)
			return g_orig_slo(InClass, InOuter, Name, Filename, LoadFlags,
			                  Sandbox, bAllowReconciliation);

		for (const auto &r : g_replaces)
		{
			if (r.original == Name)
			{
				log_info("slo_hook: replace '%ls' -> '%ls'", Name,
				         r.replacement.c_str());

				return g_orig_slo(InClass, nullptr, r.replacement.c_str(),
				                  nullptr, LoadFlags, Sandbox,
				                  bAllowReconciliation);
			}
		}

		std::wstring key = base_key(Name);
		if (!key.empty())
		{
			for (auto &e : g_spawns)
			{
				if (e.key != key)
					continue;

				void *obj = InterlockedCompareExchangePointer(&e.spawned,
				                                              nullptr, nullptr);
				if (obj)
				{
					log_info("slo_hook: spawn hit (cached) '%ls' -> %p", Name,
					         obj);
					return obj;
				}

				s_in_spawn = true;
				obj = spawn_object(e);
				s_in_spawn = false;
				if (obj)
				{
					log_info("slo_hook: spawn hit (new)    '%ls' -> %p", Name,
					         obj);
					return obj;
				}

				log_warn("slo_hook: spawn failed for '%ls', falling through",
				         Name);
				break;
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
				size_t dep_idx = it->second;
				dep_of[dep_idx].push_back(i);
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
			{
				if (--in_degree[nxt] == 0)
					queue.push_back(nxt);
			}
		}

		if (sorted.size() != n)
		{
			log_warn("mod_loader: dependency cycle detected — affected mods "
			         "appended in original order");
			for (size_t i = 0; i < n; ++i)
			{
				if (in_degree[i] > 0)
				{
					log_warn("mod_loader: cycle involves '%s'",
					         mods[i].cfg.name.c_str());
					sorted.push_back(i);
				}
			}
		}

		std::vector<LoadedMod> out;
		out.reserve(n);
		for (size_t i : sorted)
			out.push_back(std::move(mods[i]));
		return out;
	}

	static void discover_mods(const std::wstring &exe_dir)
	{
		std::wstring mods_dir = exe_dir + L"\\..\\..\\Mods";
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
			if (!parse_mod_config(to_narrow(toml_w), lm.cfg))
			{
				log_warn("mod_loader: skipping '%ls' (parse error)",
				         fd.cFileName);
				continue;
			}
			if (!lm.cfg.enabled)
			{
				log_info("mod_loader: '%ls' disabled", fd.cFileName);
				continue;
			}
			if (lm.cfg.name.empty())
				lm.cfg.name = to_narrow(fd.cFileName);
			if (lm.cfg.upk_version == 0)
				lm.cfg.upk_version = 684;

			g_mods.push_back(std::move(lm));
			log_info("mod_loader: found '%s'  v%s  by %s  deps=[%zu]",
			         g_mods.back().cfg.name.c_str(),
			         g_mods.back().cfg.version.empty()
			             ? "?"
			             : g_mods.back().cfg.version.c_str(),
			         g_mods.back().cfg.author.empty()
			             ? "unknown"
			             : g_mods.back().cfg.author.c_str(),
			         g_mods.back().cfg.dependencies.size());

		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}

	static void build_tables()
	{
		for (const auto &lm : g_mods)
		{
			for (const auto &sp : lm.cfg.spawn_patches)
			{
				if (sp.class_name.empty())
				{
					log_warn(
					    "mod_loader: [[patch.spawn]] in '%s' missing class=",
					    lm.cfg.name.c_str());
					continue;
				}
				SpawnEntry e;
				e.package_name = to_wide(
				    sp.package_name.empty() ? lm.cfg.name : sp.package_name);
				e.object_name = to_wide(
				    sp.object_name.empty() ? sp.class_name : sp.object_name);
				e.class_name = sp.class_name;
				e.key = e.package_name + L"." + e.object_name;
				g_spawns.push_back(std::move(e));
				log_info("mod_loader: spawn  '%ls'  class=%s",
				         g_spawns.back().key.c_str(), sp.class_name.c_str());
			}

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
		{
			log_warn("register_content: GPackageFileCache null, skipping '%ls'",
			         path.c_str());
			return;
		}
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
		discover_mods(get_exe_dir());
		g_mods = topo_sort(std::move(g_mods));
		build_tables();
		log_info("mod_loader: discovery done — %zu mod(s)  %zu spawn(s)"
		         "  %zu replace(s)",
		         g_mods.size(), g_spawns.size(), g_replaces.size());
	}

	void register_content()
	{
		if (!ue3().GPackageFileCache)
		{
			log_warn("register_content: GPackageFileCache not resolved"
			         " — content mods will not be registered");
			return;
		}
		void *cache = *ue3().GPackageFileCache;
		if (!cache)
		{
			log_warn("register_content: *GPackageFileCache is null"
			         " — engine may not have initialised it yet");
			return;
		}

		for (const auto &lm : g_mods)
		{
			for (const auto &rel : lm.cfg.content_paths)
			{

				std::wstring full = lm.dir_w + L"\\" + to_wide(rel);
				cache_content_path(cache, full);
			}
		}
	}

	void install_hooks()
	{
		if (!ue3().StaticLoadObject)
		{
			log_warn(
			    "mod_loader: StaticLoadObject not resolved — hook skipped");
			return;
		}

		if (g_spawns.empty() && g_replaces.empty())
		{
			log_info("mod_loader: no spawn/replace patches — SLO hook skipped");
			return;
		}
		hook::add(ue3().StaticLoadObject,
		          reinterpret_cast<void *>(&hook_StaticLoadObject),
		          reinterpret_cast<void **>(&g_orig_slo));
		log_info("mod_loader: SLO hook queued  (%zu spawns, %zu replaces)",
		         g_spawns.size(), g_replaces.size());
	}

	bool find_replace(const std::wstring &orig, std::wstring &out)
	{
		for (const auto &r : g_replaces)
		{
			if (r.original == orig)
			{
				out = r.replacement;
				return true;
			}
		}
		return false;
	}

	const std::vector<LoadedMod> &loaded_mods() { return g_mods; }

}  // namespace mod_loader
