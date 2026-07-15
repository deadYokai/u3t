#define WIN32_LEAN_AND_MEAN
#include "hook.hpp"
#include "logs.hpp"
#include "mod_loader.hpp"
#include "override_loader.hpp"
#include "ue3_layout.hpp"
#include "util.hpp"
#include <unknwn.h>
#include <windows.h>

static HMODULE g_sys_di8 = nullptr;
static FARPROC g_real_di8 = nullptr;
static HMODULE g_hmod = nullptr;

static HMODULE load_system_dinput8()
{
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

static void init_dinput8()
{
	for (const char *dep :
	     {"ole32.dll", "oleaut32.dll", "user32.dll", "advapi32.dll", "hid.dll"})
		LoadLibraryA(dep);
	g_sys_di8 = load_system_dinput8();
	if (!g_sys_di8)
		return;
	g_real_di8 = GetProcAddress(g_sys_di8, "DirectInput8Create");
	if (!g_real_di8)
	{
		log_err("DirectInput8Create not found");
		FreeLibrary(g_sys_di8);
		g_sys_di8 = nullptr;
	}
}

static DWORD WINAPI init_thread(LPVOID)
{
	logs::init(get_exe_dir());
	SYSTEMTIME st{};
	GetLocalTime(&st);

	log_info("CU3ML mod loader (%s build)",
	         sizeof(void *) == 8 ? "x64" : "x86");

	log_info("log  %04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay,
	         st.wHour, st.wMinute, st.wSecond);

	log_info("module = %p", static_cast<void *>(g_hmod));

	init_dinput8();

	log_info("init_thread: discovering mods");
	mod_loader::discover();

	log_info("init_thread: resolving UE3 layout");
	if (!ue3_resolve(ue3()))
	{
		log_err("init_thread: UE3 layout not resolved — aborting");
		return 1;
	}

	const UE3Layout &L = ue3();
	log_info("init_thread: FNameInit            = %p", L.FNameInit);
	log_info("init_thread: StaticFindObjectFast = %p", L.StaticFindObjectFast);
	log_info("init_thread: StaticLoadObject     = %p", L.StaticLoadObject);
	log_info("init_thread: Preload              = %p", L.Preload);
	log_info("init_thread: GPackageFileCache    = %p",
	         static_cast<void *>(L.GPackageFileCache));
	log_info("init_thread: FName::Names         = %p  (str_off=%zu wf=%d)",
	         static_cast<void *>(L.FNameNamesArr), L.name.str_off,
	         (int)L.name.with_flags);
	log_info("init_thread: FArchive slots       = "
	         "Serialize=%d Tell=%d Seek=%d Precache=%d SerializeName=%d "
	         "(validated=%d)",
	         L.ar.Serialize, L.ar.Tell, L.ar.Seek, L.ar.Precache,
	         L.ar.SerializeName, (int)L.ar.validated);

	log_info("init_thread: registering content paths");
	mod_loader::register_content();

	log_info("init_thread: installing redirect (SLO) hook");
	mod_loader::install_hooks();

	log_info("init_thread: discovering + installing overrides (Preload hook)");
	override_loader::discover(mod_loader::loaded_mods());
	override_loader::install_hooks();

	log_info("init_thread: committing hooks");
	hook::install_all();

	log_info("init_thread: ready");
	return 0;
}

extern "C" BOOL WINAPI DllMain(HMODULE hmod, DWORD reason, LPVOID)
{
	switch (reason)
	{
		case DLL_PROCESS_ATTACH:
			DisableThreadLibraryCalls(hmod);
			g_hmod = hmod;
			CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
			break;

		case DLL_PROCESS_DETACH:
			hook::remove_all();
			if (g_sys_di8)
			{
				FreeLibrary(g_sys_di8);
				g_sys_di8 = nullptr;
			}
			break;
	}
	return TRUE;
}

struct GUID_t
{
	UINT32 d1;
	UINT16 d2, d3;
	UINT8 d4[8];
};

using DI8Fn = HRESULT(WINAPI *)(HINSTANCE, DWORD, const GUID_t *, void **,
                                IUnknown *);

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD ver,
                                             const GUID_t *riid, void **ppv,
                                             IUnknown *outer)
{
	for (DWORD i = 0; !g_real_di8 && i < 200; ++i)
		Sleep(10);
	if (!g_real_di8)
	{
		log_err("DirectInput8Create: real fn not ready");
		return static_cast<HRESULT>(0x8007007eL);
	}
	return reinterpret_cast<DI8Fn>(g_real_di8)(hinst, ver, riid, ppv, outer);
}
