#define WIN32_LEAN_AND_MEAN
#include "blob_packer.hpp"
#include "logs.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>

namespace fs = std::filesystem;

namespace blob_packer {
namespace {

using Buf = std::vector<uint8_t>;

static void w8(Buf &b, uint8_t v) { b.push_back(v); }
static void w32(Buf &b, int32_t v) {
	w8(b, (uint8_t)v);
	w8(b, (uint8_t)(v >> 8));
	w8(b, (uint8_t)(v >> 16));
	w8(b, (uint8_t)(v >> 24));
}
static void wu32(Buf &b, uint32_t v) { w32(b, (int32_t)v); }
static void w16(Buf &b, int16_t v) {
	w8(b, (uint8_t)v);
	w8(b, (uint8_t)(v >> 8));
}
static void wu64(Buf &b, uint64_t v) {
	wu32(b, (uint32_t)v);
	wu32(b, (uint32_t)(v >> 32));
}
static void wbytes(Buf &b, const uint8_t *d, size_t n) {
	b.insert(b.end(), d, d + n);
}

static constexpr int16_t VER_HAS_GUID_OFFSETS = 623;
static constexpr int16_t VER_HAS_THUMBNAIL = 584;
static constexpr int16_t VER_HAS_EXTRA_PKGS = 516;
static constexpr int16_t VER_HAS_TEX_ALLOCS = 767;

struct NT {
	std::vector<std::string> names;

	int32_t idx(const std::string &s) const {
		for (int32_t i = 0; i < (int32_t)names.size(); ++i)
			if (names[i] == s)
				return i;
		return -1;
	}

	size_t byte_size() const {
		size_t n = 0;
		for (auto &s : names)
			n += 4 + s.size() + 1 + 8;
		return n;
	}

	void serialize(Buf &b) const {
		for (auto &s : names) {
			w32(b, (int32_t)(s.size() + 1));
			for (char c : s)
				w8(b, (uint8_t)c);
			w8(b, 0);
			wu64(b, 0); // name flags
		}
	}
};

static NT load_namemap(const std::wstring &mod_dir, const std::string &stem) {
	fs::path p = fs::path(mod_dir) / (stem + ".namemap");
	std::ifstream f(p);
	if (!f) {
		log_err("blob_packer: cannot open namemap '%ls'", p.wstring().c_str());
		return {};
	}

	NT nt;
	std::string line;
	while (std::getline(f, line)) {
		auto hash = line.find('#');
		if (hash != std::string::npos)
			line = line.substr(0, hash);
		while (!line.empty() && (line.back() == ' ' || line.back() == '\t' ||
		                         line.back() == '\r'))
			line.pop_back();
		if (!line.empty())
			nt.names.push_back(line);
	}
	return nt;
}

static size_t header_sz(int16_t ver) {
	size_t s = 0;
	s += 4 + 2 + 2; // magic + p_ver + l_ver
	s += 4;         // header_size
	s += 4;         // group/path (empty FString = i32 0)
	s += 4;         // package flags
	s += 4 * 7;     // name_cnt, name_off, exp_cnt, exp_off,
	                // imp_cnt,  imp_off,  dep_off
	if (ver >= VER_HAS_GUID_OFFSETS)
		s += 4 + 4 + 4;
	if (ver >= VER_HAS_THUMBNAIL)
		s += 4;
	s += 16;     // GUID
	s += 4 + 12; // gen_count=1 + 1 generation (3×i32)
	s += 4 + 4;  // engine_ver + cooker_ver
	s += 4 + 4;  // compression_method + compressed_chunks_count
	s += 4;      // package_source
	if (ver >= VER_HAS_EXTRA_PKGS)
		s += 4;
	if (ver >= VER_HAS_TEX_ALLOCS)
		s += 4;
	return s;
}

static void write_header(Buf &b, int16_t ver, int32_t hdr_size,
                         int32_t name_off, int32_t name_cnt, int32_t exp_off,
                         int32_t exp_cnt, int32_t imp_off, int32_t imp_cnt,
                         int32_t dep_off, int32_t guid_off) {
	wu32(b, 0x9E2A83C1);
	w16(b, ver);
	w16(b, 0);
	w32(b, hdr_size);
	w32(b, 0);  // group name (empty FString)
	wu32(b, 0); // package flags
	w32(b, name_cnt);
	w32(b, name_off);
	w32(b, exp_cnt);
	w32(b, exp_off);
	w32(b, imp_cnt);
	w32(b, imp_off);
	w32(b, dep_off);
	if (ver >= VER_HAS_GUID_OFFSETS) {
		w32(b, guid_off);
		wu32(b, 0);
		wu32(b, 0);
	}
	if (ver >= VER_HAS_THUMBNAIL)
		wu32(b, 0);
	wu32(b, 0x12345678);
	wu32(b, 0xDEADBEEF);
	wu32(b, 0xCAFEBABE);
	wu32(b, 0xFEEDFACE);
	w32(b, 1);        // generation count
	w32(b, exp_cnt);  // gen[0].export_count
	w32(b, name_cnt); // gen[0].name_count
	w32(b, 0);        // gen[0].net_guid_count
	w32(b, 0);        // engine_version
	w32(b, 12791);    // cooker_version
	wu32(b, 0);       // compression_method
	wu32(b, 0);       // compressed_chunks_count
	w32(b, 0);        // package_source
	if (ver >= VER_HAS_EXTRA_PKGS)
		w32(b, 0);
	if (ver >= VER_HAS_TEX_ALLOCS)
		w32(b, 0);
}

static void write_export(Buf &b, int32_t cls_idx, int32_t super_idx,
                         int32_t outer_idx, int32_t name_idx, int32_t name_inst,
                         int32_t archetype, uint64_t flags, int32_t ser_size,
                         int32_t ser_off) {
	w32(b, cls_idx);
	w32(b, super_idx);
	w32(b, outer_idx);
	w32(b, name_idx);
	w32(b, name_inst);
	w32(b, archetype);
	wu64(b, flags);
	w32(b, ser_size);
	w32(b, ser_off);
	wu32(b, 0); // export_flags
	w32(b, 0);  // generation net_object_count array len
	wu32(b, 0);
	wu32(b, 0);
	wu32(b, 0);
	wu32(b, 0); // package GUID
	wu32(b, 0); // package_flags
}

static void write_import(Buf &b, int32_t cls_pkg_idx, int32_t cls_name_idx,
                         int32_t outer, int32_t obj_name_idx) {
	w32(b, cls_pkg_idx);
	w32(b, 0);
	w32(b, cls_name_idx);
	w32(b, 0);
	w32(b, outer);
	w32(b, obj_name_idx);
	w32(b, 0);
}

static std::mutex g_cache_mtx;
static std::unordered_map<std::string, std::wstring> g_cache;

} // anonymous namespace

std::wstring synthesize_upk(const UPKBlobManifest &manifest) {
	{
		std::lock_guard<std::mutex> lk(g_cache_mtx);
		auto it = g_cache.find(manifest.stem);
		if (it != g_cache.end())
			return it->second;
	}

	const int16_t ver = manifest.ver;
	const fs::path dir(manifest.mod_dir);

	if (manifest.blobs.empty()) {
		log_err("blob_packer: manifest for '%s' has no blobs",
		        manifest.stem.c_str());
		return {};
	}

	NT nt = load_namemap(manifest.mod_dir, manifest.stem);
	if (nt.names.empty())
		return {};

	struct ExportInfo {
		std::string obj_name;   // stem
		std::string class_name; // extension without dot
		int32_t outer_idx;
		std::vector<uint8_t> data;
	};

	std::vector<std::string> class_order;
	auto class_import_idx = [&](const std::string &cls) -> int32_t {
		for (size_t i = 0; i < class_order.size(); ++i)
			if (class_order[i] == cls)
				return -(int32_t)(i + 2);
		class_order.push_back(cls);
		return -(int32_t)(class_order.size() + 1);
	};

	std::vector<ExportInfo> exports;
	exports.reserve(manifest.blobs.size());

	for (size_t i = 0; i < manifest.blobs.size(); ++i) {
		fs::path bp = dir / manifest.blobs[i];
		std::string ext = bp.extension().string();
		std::string obj = bp.stem().string();
		std::string cls =
		    (ext.size() > 1 && ext[0] == '.') ? ext.substr(1) : ext;

		if (cls.empty()) {
			log_err("blob_packer: blob '%s' has no extension (need class name)",
			        manifest.blobs[i].c_str());
			return {};
		}

		std::ifstream f(bp, std::ios::binary);
		if (!f) {
			log_err("blob_packer: cannot read blob '%ls'",
			        bp.wstring().c_str());
			return {};
		}
		std::vector<uint8_t> data(std::istreambuf_iterator<char>(f), {});
		if (data.empty()) {
			log_err("blob_packer: blob '%ls' is empty", bp.wstring().c_str());
			return {};
		}

		int32_t outer = (i == 0) ? 0 : 1;
		if (i < manifest.outers.size())
			outer = manifest.outers[i];

		class_import_idx(cls);

		exports.push_back({obj, cls, outer, std::move(data)});
	}

	const auto check_name = [&](const std::string &n) -> bool {
		if (nt.idx(n) < 0) {
			log_err("blob_packer: name '%s' missing from '%s.namemap'",
			        n.c_str(), manifest.stem.c_str());
			return false;
		}
		return true;
	};
	bool ok = check_name("None") && check_name(manifest.stem) &&
	          check_name("Core") && check_name("Class") &&
	          check_name("Package") && check_name("Engine");
	for (auto &cls : class_order)
		ok = ok && check_name(cls);
	if (!ok)
		return {};

	constexpr size_t kExpSz = 68;
	constexpr size_t kImpSz = 28;
	const size_t n_imp = 1 + class_order.size(); // Engine + one per class
	const size_t n_exp = exports.size();

	const size_t h_sz = header_sz(ver);
	const size_t nt_sz = nt.byte_size();
	const size_t exp_sz = n_exp * kExpSz;
	const size_t imp_sz = n_imp * kImpSz;
	const size_t dep_sz = n_exp * 4;

	const int32_t name_off = (int32_t)h_sz;
	const int32_t export_off = (int32_t)(h_sz + nt_sz);
	const int32_t import_off = (int32_t)(h_sz + nt_sz + exp_sz);
	const int32_t depend_off = (int32_t)(h_sz + nt_sz + exp_sz + imp_sz);
	const int32_t guid_off = depend_off + (int32_t)dep_sz;
	const int32_t serial_base = guid_off;
	const int32_t hdr_size = serial_base;

	std::vector<int32_t> ser_offs;
	int32_t cur = serial_base;
	for (auto &e : exports) {
		ser_offs.push_back(cur);
		cur += (int32_t)e.data.size();
	}

	Buf upk;
	upk.reserve((size_t)cur);

	write_header(upk, ver, hdr_size, name_off, (int32_t)nt.names.size(),
	             export_off, (int32_t)n_exp, import_off, (int32_t)n_imp,
	             depend_off, guid_off);

	if (upk.size() != h_sz) {
		log_err("blob_packer: header size mismatch: wrote %zu expected %zu "
		        "(ver=%d) — update header_sz()",
		        upk.size(), h_sz, (int)ver);
		return {};
	}

	nt.serialize(upk);

	for (size_t i = 0; i < exports.size(); ++i) {
		auto &e = exports[i];
		int32_t cls_imp = class_import_idx(e.class_name);

		uint64_t flags =
		    (e.outer_idx == 0) ? 0x000000000000000CULL : 0x0000000000000004ULL;

		write_export(upk, cls_imp, 0, e.outer_idx, nt.idx(e.obj_name), 0, 0,
		             flags, (int32_t)e.data.size(), ser_offs[i]);
	}

	write_import(upk, nt.idx("Core"), nt.idx("Package"), 0, nt.idx("Engine"));
	for (auto &cls : class_order)
		write_import(upk, nt.idx("Core"), nt.idx("Class"), -1, nt.idx(cls));

	for (size_t i = 0; i < n_exp; ++i)
		w32(upk, 0);

	for (auto &e : exports)
		wbytes(upk, e.data.data(), e.data.size());

	wchar_t tmp[MAX_PATH];
	GetTempPathW(MAX_PATH, tmp);
	const std::wstring out_path =
	    std::wstring(tmp) + L"ue3mod_" +
	    std::wstring(manifest.stem.begin(), manifest.stem.end()) + L".upk";

	{
		std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
		if (!f) {
			log_err("blob_packer: cannot write '%ls'", out_path.c_str());
			return {};
		}
		f.write(reinterpret_cast<const char *>(upk.data()),
		        (std::streamsize)upk.size());
	}

	log_info("blob_packer: synthesized '%ls'  (%zu B, %zu export(s), ver=%d)",
	         out_path.c_str(), upk.size(), n_exp, (int)ver);

	std::lock_guard<std::mutex> lk(g_cache_mtx);
	g_cache[manifest.stem] = out_path;
	return out_path;
}

} // namespace blob_packer
