#define WIN32_LEAN_AND_MEAN
#include "linker_hook.hpp"
#include "disasm.hpp"
#include "logs.hpp"
#include "mod_loader.hpp"
#include <atomic>
#include <cstring>
#include <mutex>
#include <psapi.h>
#include <string>
#include <unordered_set>
#include <windows.h>

namespace {

std::unordered_set<int32_t> g_seen_names;
std::mutex g_seen_mtx;

static std::string build_path(void *outer_obj, FName leaf) {
	std::string leaf_s = fname_to_string(leaf);
	if (leaf_s.empty())
		return {};

	std::string outers[3];
	int n = 0;
	void *cur = outer_obj;
	while (cur && n < 3) {
		FName fn{};
		memcpy(&fn, static_cast<uint8_t *>(cur) + UObjectOff::ObjectName,
		       sizeof(FName));
		std::string s = fname_to_string(fn);
		if (s.empty())
			break;
		outers[n++] = std::move(s);
		void *next = nullptr;
		memcpy(&next, static_cast<uint8_t *>(cur) + UObjectOff::Outer,
		       sizeof(void *));
		cur = next;
	}

	std::string path;
	for (int i = n - 1; i >= 0; --i) {
		path += outers[i];
		path += '.';
	}
	path += leaf_s;
	return path;
}

static void discovery_log(void *cls, void *outer, FName name) {
	if (!ue3().FNameNames)
		return;
	if (name.Index == 0)
		return;

	{
		std::lock_guard<std::mutex> lk(g_seen_mtx);
		if (!g_seen_names.insert(name.Index).second)
			return;
	}

	std::string path = build_path(outer, name);
	if (path.empty())
		path = fname_to_string(name);
	if (path.empty())
		return;

	std::string cls_name;
	if (cls) {
		FName cls_fname{};
		memcpy(&cls_fname, static_cast<uint8_t *>(cls) + UObjectOff::ObjectName,
		       sizeof(FName));
		cls_name = fname_to_string(cls_fname);
	}

	if (cls_name.empty())
		log_info("[disc] %s", path.c_str());
	else
		log_info("[disc] %s  (%s)", path.c_str(), cls_name.c_str());
}

} // namespace

TrampolineHook Hook_StaticFindObjectFast{};

static bool outer_isPackage(void *obj) {
	if (!obj)
		return false;

	void *cls = nullptr;
	memcpy(&cls, static_cast<uint8_t *>(obj) + UObjectOff::Class,
	       sizeof(void *));
	if (!cls)
		return false;

	FName cls_fname{};
	memcpy(&cls_fname, static_cast<uint8_t *>(cls) + UObjectOff::ObjectName,
	       sizeof(FName));

	static std::atomic<int32_t> s_pkg_idx{-1};
	int32_t pkg_idx = s_pkg_idx.load(std::memory_order_relaxed);
	if (pkg_idx < 0) {
		if (!ue3().FNameInit)
			return false;
		FName fn = fname_find(L"Package");
		if (fn.Index > 0) {
			s_pkg_idx.store(fn.Index, std::memory_order_relaxed);
			pkg_idx = fn.Index;
		}
	}

	return pkg_idx > 0 && cls_fname.Index == pkg_idx;
}

void *__fastcall Hooked_StaticFindObjectFast(void *cls, void *outer, FName name,
                                             int bExact, int bAny,
                                             uint64_t excl) {
	linker_hook::ensure_vtable_hook();

	// discovery_log(cls, outer, name);

	if (!outer_isPackage(outer)) {
		void *repl = mod_loader::find_replacement(name, outer, cls);
		if (repl)
			return repl;
	}

	return reinterpret_cast<StaticFindObjectFast_fn>(
	    Hook_StaticFindObjectFast.trampoline)(cls, outer, name, bExact, bAny,
	                                          excl);
}

namespace linker_hook {
namespace {

static FindPackageFile_fn g_orig_fpf = nullptr;
static void **g_fpf_slot = nullptr;
static std::atomic<bool> g_installed{false};
static std::mutex g_install_mtx;

static constexpr int kFPFSlot = 2;
static constexpr int kVtblScan = 16;

static bool is_readable(const void *addr, size_t size = sizeof(void *)) {
	if (!addr)
		return false;
	MEMORY_BASIC_INFORMATION mbi{};
	if (!VirtualQuery(addr, &mbi, sizeof(mbi)))
		return false;
	if (mbi.State != MEM_COMMIT)
		return false;
	if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
		return false;
	constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE |
	                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
	                            PAGE_EXECUTE_WRITECOPY | PAGE_WRITECOPY;
	if (!(mbi.Protect & kReadable))
		return false;
	return (reinterpret_cast<uintptr_t>(addr) + size) <=
	       (reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize);
}

static int __fastcall hook_fpf(void *self, const wchar_t *name, void *guid,
                               FStringLayout *out_fstr,
                               const wchar_t *language) {
	std::wstring mod_path = mod_loader::find_mod_pkg_path(name);
	if (!mod_path.empty()) {
		static wchar_t s_buf[512];
		wcsncpy_s(s_buf, mod_path.c_str(), 511);
		out_fstr->Data = s_buf;
		out_fstr->Num = static_cast<int32_t>(wcslen(s_buf) + 1);
		out_fstr->Max = 0;
		log_info("linker_hook: FindPackageFile '%ls' -> mod path", name);
		return 1;
	}
	return g_orig_fpf(self, name, guid, out_fstr, language);
}

static bool vtable_write(void **slot, void *fn) {
	DWORD old{};
	if (VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) {
		*slot = fn;
		VirtualProtect(slot, sizeof(void *), old, &old);
		return true;
	}
	SIZE_T written = 0;
	WriteProcessMemory(GetCurrentProcess(), slot, &fn, sizeof(fn), &written);
	return written == sizeof(fn);
}

static bool do_install_vtable() {
	void **cache = ue3().GPackageFileCache;
	if (!cache || !*cache)
		return false;

	void *obj = *cache;
	log_info("linker_hook: GPackageFileCache var=%p  obj=%p", cache, obj);

	if (!is_readable(obj)) {
		log_err("linker_hook: GPackageFileCache object %p is not readable "
		        "— RIP displacement wrong; update PATTERN_GetPackageLinker",
		        obj);
		return false;
	}

	auto **vtbl = *reinterpret_cast<void ***>(obj);
	log_info("linker_hook: vtbl = %p", vtbl);

	if (!is_readable(vtbl, sizeof(void *) * (kFPFSlot + 1))) {
		log_err("linker_hook: vtbl %p is not readable — "
		        "%p does not look like a FPackageFileCache",
		        vtbl, obj);
		return false;
	}

	const int slot = kFPFSlot;

	g_fpf_slot = &vtbl[slot];
	g_orig_fpf = reinterpret_cast<FindPackageFile_fn>(*g_fpf_slot);

	if (!vtable_write(g_fpf_slot, reinterpret_cast<void *>(&hook_fpf))) {
		log_err("linker_hook: vtable write failed for slot %d", slot);
		g_fpf_slot = nullptr;
		g_orig_fpf = nullptr;
		return false;
	}

	log_info("linker_hook: FindPackageFile hooked  slot=%d  orig=%p", slot,
	         reinterpret_cast<void *>(g_orig_fpf));
	return true;
}

} // namespace

void ensure_vtable_hook() {
	if (g_installed.load(std::memory_order_acquire))
		return;

	if (!ue3().GPackageFileCache || !*ue3().GPackageFileCache)
		return;

	std::lock_guard<std::mutex> lk(g_install_mtx);
	if (g_installed.load(std::memory_order_relaxed))
		return;

	if (do_install_vtable()) {
		g_installed.store(true, std::memory_order_release);
	}
}

void remove_vtable() {
	if (!g_fpf_slot || !g_orig_fpf)
		return;
	vtable_write(g_fpf_slot, reinterpret_cast<void *>(g_orig_fpf));
	g_fpf_slot = nullptr;
	g_orig_fpf = nullptr;
	g_installed.store(false, std::memory_order_release);
	log_info("linker_hook: FindPackageFile vtable hook removed");
}

} // namespace linker_hook
