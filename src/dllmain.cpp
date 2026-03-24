#define WIN32_LEAN_AND_MEAN
#include "hook.hpp"
#include "logs.hpp"
#include "mod_loader.hpp"
#include "ue3_types.hpp"
#include "util.hpp"
#include <unknwn.h>
#include <windows.h>

static HMODULE g_sys_di8 = nullptr;
static FARPROC g_real_di8 = nullptr;

static HMODULE load_system_dinput8() {
	char sys[MAX_PATH]{};
#ifdef _WIN64
	GetSystemDirectoryA(sys, MAX_PATH);
#else
	BOOL wow = FALSE;
	{
		using Fn = BOOL(WINAPI *)(HANDLE, PBOOL);
		auto fn = reinterpret_cast<Fn>(
		    GetProcAddress(GetModuleHandleA("kernel32"), "IsWow64Process"));
		if (fn)
			fn(GetCurrentProcess(), &wow);
	}
	if (wow)
		GetSystemWow64DirectoryA(sys, MAX_PATH);
	else
		GetSystemDirectoryA(sys, MAX_PATH);
#endif
	char path[MAX_PATH]{};
	snprintf(path, MAX_PATH, "%s\\dinput8.dll", sys);
	log_info("loading real dinput8: %s", path);
	HMODULE h = LoadLibraryA(path);
	if (!h)
		log_err("LoadLibraryA failed (err=%lu)", GetLastError());
	return h;
}

static void init_dinput8() {
	for (const char *dep :
	     {"ole32.dll", "oleaut32.dll", "user32.dll", "advapi32.dll", "hid.dll"})
		LoadLibraryA(dep);
	g_sys_di8 = load_system_dinput8();
	if (!g_sys_di8)
		return;
	g_real_di8 = GetProcAddress(g_sys_di8, "DirectInput8Create");
	if (!g_real_di8) {
		log_err("DirectInput8Create not found");
		FreeLibrary(g_sys_di8);
		g_sys_di8 = nullptr;
	}
}

static DWORD WINAPI init_thread(LPVOID) {
	log_info("init_thread: scanning mod data");
	mod_loader::ensure_loaded();

	log_info("init_thread: resolving UE3 addresses");
	if (!ue3_resolve(ue3())) {
		log_err("init_thread: one or more UE3 addresses not found — aborting");
		return 1;
	}
	log_info("init_thread: FNameInit            = %p",
	         reinterpret_cast<void *>(ue3().FNameInit));
	log_info("init_thread: StaticFindObjectFast = %p",
	         reinterpret_cast<void *>(ue3().StaticFindObjectFast));
	log_info("init_thread: StaticLoadObject     = %p",
	         reinterpret_cast<void *>(ue3().StaticLoadObject));
	log_info("init_thread: GPackageFileCache    = %p",
	         static_cast<void *>(ue3().GPackageFileCache));
	if (ue3().FNameNames) {
		log_info("init_thread: FNameNames           = %p  (Num=%d)",
		         static_cast<void *>(ue3().FNameNames), ue3().FNameNames->Num);
	} else {
		log_warn("init_thread: FNameNames           = NULL  "
		         "— discovery log will be silent; "
		         "FNameInit body scan found no valid Names array");
	}
	log_info("init_thread: installing hooks");
	hook::install_all();

	log_info("init_thread: ready — all remaining init is lazy");
	return 0;
}

extern "C" BOOL WINAPI DllMain(HMODULE hmod, DWORD reason, LPVOID) {
	switch (reason) {
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(hmod);
		logs::init(get_exe_dir());
		log_info("U3T mod loader (%s build)",
		         sizeof(void *) == 8 ? "x64" : "x86");
		log_info("module = %p", static_cast<void *>(hmod));
		init_dinput8();
		CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
		break;

	case DLL_PROCESS_DETACH:
		hook::remove_all();
		if (g_sys_di8) {
			FreeLibrary(g_sys_di8);
			g_sys_di8 = nullptr;
		}
		break;
	}
	return TRUE;
}

struct GUID_t {
	UINT32 d1;
	UINT16 d2, d3;
	UINT8 d4[8];
};
using DI8Fn = HRESULT(WINAPI *)(HINSTANCE, DWORD, const GUID_t *, void **,
                                IUnknown *);

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD ver,
                                             const GUID_t *riid, void **ppv,
                                             IUnknown *outer) {
	for (DWORD i = 0; !g_real_di8 && i < 200; ++i)
		Sleep(10);
	if (!g_real_di8) {
		log_err("DirectInput8Create: real fn not ready");
		return static_cast<HRESULT>(0x8007007eL);
	}
	return reinterpret_cast<DI8Fn>(g_real_di8)(hinst, ver, riid, ppv, outer);
}
