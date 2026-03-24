#define WIN32_LEAN_AND_MEAN
#include "mod_loader.hpp"
#include "blob_packer.hpp"
#include "logs.hpp"
#include "mod_toml_mini.hpp"
#include "util.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <windows.h>

namespace fs = std::filesystem;
namespace {

std::once_flag g_once;
std::vector<ObjectReplacement> g_replacements;
std::vector<UPKBlobManifest> g_manifests;
std::mutex g_load_mtx;

static void *resolve_ue3_class(ObjectReplacement &r) {
	if (r.cls_resolved)
		return r.cached_cls;
	if (!ue3().StaticFindObjectFast)
		return nullptr;

	FName cls_name = fname_create(to_wide(r.ue3_class).c_str());
	if (cls_name.Index == 0)
		return nullptr;

	void *cls = ue3().StaticFindObjectFast(nullptr, nullptr, cls_name, 0, 1, 0);
	if (!cls)
		return nullptr;

	log_info("mod_loader: UClass '%s' resolved -> %p", r.ue3_class.c_str(),
	         cls);

	static std::once_flag s_manifest;
	std::call_once(s_manifest, [&] {
		log_info(
		    "mod_loader: ── mod manifest (%zu entries) ────────────────────",
		    g_replacements.size());
		for (size_t i = 0; i < g_replacements.size(); ++i) {
			const auto &e = g_replacements[i];
			log_info("mod_loader:   [%zu] orig='%s'  class='%s'  repl='%ls'%s",
			         i, e.orig_full_path.c_str(), e.ue3_class.c_str(),
			         e.repl_path_w.c_str(), e.is_blob ? "  [blob]" : "");
		}
		log_info(
		    "mod_loader: ─────────────────────────────────────────────────");
	});

	r.cached_cls = cls;
	r.cls_resolved = true;
	return cls;
}

static void load_mod_dir(const fs::path &dir) {
	auto toml = dir / "mod.toml";
	if (!fs::exists(toml))
		return;

	std::ifstream f(toml);
	if (!f) {
		log_warn("mod_loader: cannot open %s", toml.string().c_str());
		return;
	}
	std::string text((std::istreambuf_iterator<char>(f)), {});

	auto res = mod_toml_mini::parse(text, dir.string());

	for (auto &m : res.manifests) {
		log_info("mod_loader: upk-manifest  stem='%s'  blobs=%zu  ver=%d",
		         m.stem.c_str(), m.blobs.size(), (int)m.ver);
		g_manifests.push_back(std::move(m));
	}

	for (auto &r : res.replacements) {
		log_info("mod_loader: queued  orig='%s'  repl='%ls'  class='%s'%s",
		         r.orig_full_path.c_str(), r.repl_path_w.c_str(),
		         r.ue3_class.c_str(), r.is_blob ? "  [blob→synth]" : "");
		g_replacements.push_back(std::move(r));
	}

	log_info("mod_loader: loaded '%s'  (%zu replacement(s), %zu manifest(s))",
	         dir.filename().string().c_str(), res.replacements.size(),
	         res.manifests.size());
}

static void do_load() {
	std::wstring exe_dir = get_exe_dir();
	if (exe_dir.empty()) {
		log_err("mod_loader: get_exe_dir failed");
		return;
	}

	fs::path mods = fs::path(exe_dir) / ".." / ".." / L"Mods";
	if (!fs::exists(mods)) {
		log_info("mod_loader: no Mods directory at '%s'",
		         mods.string().c_str());
		return;
	}
	log_info("mod_loader: scanning '%s'", mods.string().c_str());

	try {
		for (const auto &e : fs::directory_iterator(mods))
			if (e.is_directory())
				load_mod_dir(e.path());
	} catch (const std::exception &ex) {
		log_err("mod_loader: scan error: %s", ex.what());
	}

	log_info("mod_loader: ready — %zu replacement(s), %zu manifest(s)",
	         g_replacements.size(), g_manifests.size());
}

static void *load_one(ObjectReplacement &r, void *outer, void * /*hook_cls*/) {
	if (!ue3().StaticLoadObject)
		return nullptr;

	void *cls = resolve_ue3_class(r);
	if (!cls)
		return nullptr;

	if (!r.orig_found) {
		if (outer) {
			r.orig_found = r.orig_warned = true;
			log_info("mod_loader: original '%s' confirmed via hook outer=%p",
			         r.orig_full_path.c_str(), outer);
		} else if (!r.orig_warned) {
			r.orig_warned = true;
			log_warn("mod_loader: original '%s' triggered with null outer "
			         "— bAnyPackage call, proceeding anyway",
			         r.orig_full_path.c_str());
		}
	}

	log_info("mod_loader: StaticLoadObject  cls=%p  path='%ls'", cls,
	         r.repl_path_w.c_str());

	void *result = ue3().StaticLoadObject(cls, nullptr, r.repl_path_w.c_str(),
	                                      nullptr, 0, nullptr, 1);

	if (!result) {
		if (!r.slo_failed) {
			r.slo_failed = true;
			log_err("mod_loader: StaticLoadObject FAILED for '%ls' "
			        "(cls=%p  outer=%p) — bad path, missing/corrupt file, "
			        "or engine not ready; will keep retrying",
			        r.repl_path_w.c_str(), cls, outer);
		}
		return nullptr;
	}

	if (r.slo_failed) {
		r.slo_failed = false;
		log_info("mod_loader: StaticLoadObject recovered for '%ls' -> %p",
		         r.repl_path_w.c_str(), result);
	} else {
		log_info("mod_loader: loaded replacement '%ls' -> %p",
		         r.repl_path_w.c_str(), result);
	}
	return result;
}

} // namespace

namespace mod_loader {

void ensure_loaded() { std::call_once(g_once, do_load); }

void *find_replacement(FName name, void *outer, void *hook_cls) {
	if (name.Index == 0 || g_replacements.empty())
		return nullptr;

	thread_local bool s_in_find = false;
	if (s_in_find)
		return nullptr;

	for (auto &r : g_replacements) {
		if (r.orig_name.Index == 0) {
			r.orig_name = fname_create(to_wide(r.orig_obj).c_str());
			if (r.orig_name.Index == 0)
				continue;
		}
		if (r.orig_name.Index != name.Index)
			continue;

		if (!r.cached_obj) {
			std::lock_guard<std::mutex> lk(g_load_mtx);
			if (r.cached_obj)
				return r.cached_obj;
			s_in_find = true;
			r.cached_obj = load_one(r, outer, hook_cls);
			s_in_find = false;
		}
		return r.cached_obj;
	}
	return nullptr;
}

std::wstring find_mod_pkg_path(const wchar_t *pkg_name) {
	std::string needle;
	for (const wchar_t *p = pkg_name; *p; ++p)
		needle += static_cast<char>(towlower(*p));

	for (auto &r : g_replacements) {
		if (r.mod_upk_w.empty())
			continue;
		std::string stem = fs::path(r.mod_upk_w).stem().string();
		for (auto &c : stem)
			c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
		if (stem == needle)
			return r.mod_upk_w;
	}

	for (auto &m : g_manifests) {
		std::string stem_lo = m.stem;
		for (auto &c : stem_lo)
			c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
		if (stem_lo != needle)
			continue;

		std::wstring synth = blob_packer::synthesize_upk(m);
		if (synth.empty())
			return {};

		{
			std::lock_guard<std::mutex> lk(g_load_mtx);
			for (auto &r : g_replacements)
				if (r.is_blob && r.blob_stem == m.stem)
					r.mod_upk_w = synth;
		}
		return synth;
	}
	return {};
}

} // namespace mod_loader
