#define WIN32_LEAN_AND_MEAN
#include "addr_cache.hpp"
#include "anchor.hpp"
#include "hook.hpp"
#include "loader_config.hpp"
#include "logs.hpp"
#include "lua_host.hpp"
#include "mod_loader.hpp"
#include "override_loader.hpp"
#include "ue3_api.hpp"
#include "ue3_layout.hpp"
#include "util.hpp"

#include "manager/ui.hpp"

#include <unknwn.h>
#include <windows.h>

static HMODULE g_sys_di8 = nullptr;
static FARPROC g_real_di8 = nullptr;
static HMODULE g_hmod = nullptr;

using EngineLoopInitFn = int(UE3_THISCALL *)(void *);
static EngineLoopInitFn g_orig_engine_loop_init = nullptr;

static HMODULE load_system_dinput8()
{
	std::wstring orig = get_exe_dir() + L"\\dinput8.orig.dll";

	DWORD attr = GetFileAttributesW(orig.c_str());
	if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
	{
		log_info("loading local original dinput8: %ls", orig.c_str());

		HMODULE h = LoadLibraryW(orig.c_str());
		if (h)
			return h;

		log_err("LoadLibraryW(%ls) failed (err=%lu)", orig.c_str(),
		        GetLastError());
	}

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

std::wstring lower(std::wstring s)
{
	for (auto &c : s)
		c = (wchar_t)towlower(c);
	return s;
}

const std::vector<std::wstring> &tokens()
{
	static std::vector<std::wstring> toks;
	static bool done = false;
	if (done)
		return toks;
	done = true;

	const wchar_t *raw = GetCommandLineW();
	if (!raw)
		return toks;

	std::wstring cur;
	bool in_quote = false;
	for (const wchar_t *p = raw; *p; ++p)
	{
		if (*p == L'"')
		{
			in_quote = !in_quote;
			continue;
		}
		if (!in_quote && (*p == L' ' || *p == L'\t'))
		{
			if (!cur.empty())
				toks.push_back(cur);
			cur.clear();
			continue;
		}
		cur += *p;
	}
	if (!cur.empty())
		toks.push_back(cur);
	return toks;
}

bool cmdline_has_switch(const wchar_t *name)
{
	std::wstring want = lower(name);
	for (const auto &t : tokens())
	{
		if (t.size() < 2 || (t[0] != L'-' && t[0] != L'/'))
			continue;
		if (lower(t.substr(1)) == want)
			return true;
	}
	return false;
}

bool cmdline_get_value(const wchar_t *name, std::wstring &out)
{
	std::wstring want = lower(name) + L"=";
	for (const auto &t : tokens())
	{
		if (t.size() < 2 || (t[0] != L'-' && t[0] != L'/'))
			continue;
		std::wstring body = t.substr(1);
		if (lower(body).rfind(want, 0) != 0)
			continue;
		out = body.substr(want.size());
		return !out.empty();
	}
	return false;
}

static int __fastcall engine_loop_init(void *this_ptr)
{

	if (cmdline_has_switch(L"cu3ml-manager") ||
	    loader_config::settings().manager_at_start)
	{
		log_info("engine_loop_init: manager requested — halting boot");

		ui::Result r = ui::run();
		if (r == ui::Result::Quit)
		{
			log_info("engine_loop_init: manager requested exit");
			hook::remove_all();
			ExitProcess(0);
		}

		mod_loader::refresh();
		log_info("engine_loop_init: manager requested launch — resuming");
	}

	log_info("engine_loop_init: GEngineLoop::Init reached — resolving "
	         "UE3 layout");

	if (!ue3_resolve(ue3()))
	{
		log_err("engine_loop_init: UE3 layout not resolved — mods "
		        "will not load");
	}
	else
	{
		const UE3Layout &L = ue3();
		log_info("engine_loop_init: FNameInit            = %p", L.FNameInit);
		log_info("engine_loop_init: StaticFindObjectFast = %p",
		         L.StaticFindObjectFast);
		log_info("engine_loop_init: StaticLoadObject     = %p",
		         L.StaticLoadObject);
		log_info("engine_loop_init: Preload              = %p", L.Preload);
		log_info("engine_loop_init: GPackageFileCache    = %p",
		         static_cast<void *>(L.GPackageFileCache));
		log_info("engine_loop_init: FName::Names         = %p  "
		         "(str_off=%zu wf=%d)",
		         static_cast<void *>(L.FNameNamesArr), L.name.str_off,
		         (int)L.name.with_flags);
		log_info("engine_loop_init: FArchive slots       = "
		         "Serialize=%d Tell=%d Seek=%d Precache=%d SerializeName=%d "
		         "(validated=%d)",
		         L.ar.Serialize, L.ar.Tell, L.ar.Seek, L.ar.Precache,
		         L.ar.SerializeName, (int)L.ar.validated);

		log_info("engine_loop_init: registering content paths");
		mod_loader::register_content();

		log_info("engine_loop_init: installing redirect (SLO) hook");
		mod_loader::install_hooks();

		log_info("engine_loop_init: discovering + installing "
		         "overrides (Preload hook)");
		override_loader::discover(mod_loader::enabled_mods());
		override_loader::install_hooks();

		log_info("engine_loop_init: committing hooks");
		hook::install_all();

		lua_host::init();

		log_info("engine_loop_init: ready");
	}

	return g_orig_engine_loop_init(this_ptr);
}

static bool install_engine_loop_init_hook()
{

	void *target = nullptr;

	if (addr_cache::get_ptr("engine.EngineLoopInit", target) && target)
	{
		log_info("install_engine_loop_init_hook: GEngineLoop::Init = %p "
		         "(from cu3ml.addrlist)",
		         target);
	}
	else
	{
		anchor::ModuleImage img = anchor::image_of(nullptr);
		if (!img.ok)
		{
			log_err("install_engine_loop_init_hook: image_of failed");
			return false;
		}

		auto hits =
		    anchor::functions_referencing_wstr(img, L"NoTextureStreaming");
		if (hits.size() != 1)
		{
			log_err("install_engine_loop_init_hook: anchor ambiguous/missing "
			        "(%zu hits) — check this build",
			        hits.size());
			return false;
		}

		target = hits.front();
		log_info("install_engine_loop_init_hook: GEngineLoop::Init = %p "
		         "(scanned)",
		         target);

		addr_cache::put_ptr("engine.EngineLoopInit", target);
		addr_cache::save();
	}

	hook::add(target, reinterpret_cast<void *>(&engine_loop_init),
	          reinterpret_cast<void **>(&g_orig_engine_loop_init));
	log_info("engine_loop_init: committing hooks");
	hook::install_all();

	log_info("engine_loop_init: ready");

	return g_orig_engine_loop_init != nullptr;
}

static DWORD WINAPI init_thread(LPVOID)
{
	logs::init();
	addr_cache::init();
	loader_config::load();
	SYSTEMTIME st{};
	GetLocalTime(&st);

	log_info("CU3ML mod loader (%s build)",
	         sizeof(void *) == 8 ? "x64" : "x86");

	log_info("log  %04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay,
	         st.wHour, st.wMinute, st.wSecond);

	log_info("module = %p", static_cast<void *>(g_hmod));

	lua_host::configure_from_cmdline();

	init_dinput8();

	log_info("init_thread: discovering mods");
	mod_loader::discover();

	log_info("init_thread: installing GEngineLoop::Init hook");
	if (!install_engine_loop_init_hook())
	{
		log_err(
		    "init_thread: GEngineLoop::Init hook install failed — aborting");
		return 1;
	}

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
