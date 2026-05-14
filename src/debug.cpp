#define WIN32_LEAN_AND_MEAN
#include "debug.hpp"
#include "hook.hpp"
#include "logs.hpp"
#include "pattern_scanner.hpp"
#include <cstdint>
#include <windows.h>

static constexpr bool kEnable_Tick = false;
static constexpr bool kEnable_Create = true;
static constexpr bool kEnable_CreateExport = false;
static constexpr bool kEnable_SerializePFS = false;
static constexpr bool kEnable_PreLoadObjs = false;
static constexpr bool kEnable_FAsyncTick = false;

static const char *kPat_Tick =
    "48 89 5C 24 10 57 48 83 EC 30 83 B9 90 06 00 00 00";

static const char *kPat_Create =
    "4C 89 44 24 18 53 55 56 57 41 54 41 55 41 56 48 81 EC 90 00 00 00";

static const char *kPat_CreateExport =
    "89 54 24 10 48 89 4C 24 08 53 55 56 57 41 54 41 55 41 56 41 57 48 81 EC "
    "A8 00 00 00";

static const char *kPat_SerializePackageFileSummary =
    "48 8B C4 57 41 54 41 55 41 56 41 57 48 83 EC 60 48 C7 44 24 30 FE FF FF "
    "FF";

static const char *kPat_PreLoadObjects =
    "48 89 74 24 20 57 48 83 EC 20 8B 05 ?? ?? ?? ?? 33 F6 48 8B F9 3B 41 48";

static const char *kPat_FAsyncTick =
    "53 48 83 EC 30 83 3D ?? ?? ?? ?? 00 48 8B D9";

struct ULinkerLoad_opaque;
struct UObject_opaque;
struct FAsyncPackage_opaque;

using Fn_Tick = int(__cdecl *)(ULinkerLoad_opaque *, float, int);
using Fn_Create = UObject_opaque *(__cdecl *)(ULinkerLoad_opaque *, uintptr_t,
                                              uintptr_t, uintptr_t, uint32_t,
                                              int);
using Fn_Preload = void(__cdecl *)(ULinkerLoad_opaque *, UObject_opaque *);
using Fn_CreateExport = UObject_opaque *(__cdecl *)(UObject_opaque *, int);
using Fn_SerializePFS = int(__cdecl *)(ULinkerLoad_opaque *);
using Fn_PreLoadObjects = int(__cdecl *)(FAsyncPackage_opaque *);
using Fn_FAsyncTick = int(__cdecl *)(FAsyncPackage_opaque *, int, float);

static Fn_Tick g_orig_Tick = nullptr;
static Fn_Create g_orig_Create = nullptr;
static Fn_Preload g_orig_Preload = nullptr;
static Fn_CreateExport g_orig_CE = nullptr;
static Fn_SerializePFS g_orig_SPFS = nullptr;
static Fn_PreLoadObjects g_orig_PLO = nullptr;
static Fn_FAsyncTick g_orig_FAT = nullptr;

static thread_local int s_preload_depth = 0;

static int __cdecl dbg_Tick(ULinkerLoad_opaque *L, float tl, int ul)
{
	log_info(">> ULinkerLoad::Tick  linker=%p  limit=%.4f  use=%d", (void *)L,
	         (double)tl, ul);
	int r = g_orig_Tick(L, tl, ul);
	log_info("<< ULinkerLoad::Tick  linker=%p  -> %d", (void *)L, r);
	return r;
}

static UObject_opaque *__cdecl dbg_Create(ULinkerLoad_opaque *L, uintptr_t cls,
                                          uintptr_t nm, uintptr_t outer,
                                          uint32_t flags, int silent)
{
	log_info(">> ULinkerLoad::Create  linker=%p  cls=%p  name=%p  "
	         "outer=%p  flags=0x%x  silent=%d",
	         (void *)L, (void *)cls, (void *)nm, (void *)outer, flags, silent);
	auto *r = g_orig_Create(L, cls, nm, outer, flags, silent);
	log_info("<< ULinkerLoad::Create  linker=%p  -> %p", (void *)L, (void *)r);
	return r;
}

static void __cdecl dbg_Preload(ULinkerLoad_opaque *L, UObject_opaque *obj)
{
	++s_preload_depth;
	log_info(">> ULinkerLoad::Preload  [depth=%d]  linker=%p  obj=%p",
	         s_preload_depth, (void *)L, (void *)obj);
	g_orig_Preload(L, obj);
	log_info("<< ULinkerLoad::Preload  [depth=%d]  linker=%p  obj=%p",
	         s_preload_depth, (void *)L, (void *)obj);
	--s_preload_depth;
}

static UObject_opaque *__cdecl dbg_CreateExport(UObject_opaque *L, int idx)
{
	log_info(">> ULinkerLoad::CreateExport  linker=%p  index=%d", (void *)L,
	         idx);
	auto *r = g_orig_CE(L, idx);
	log_info("<< ULinkerLoad::CreateExport  linker=%p  -> %p", (void *)L,
	         (void *)r);
	return r;
}

static int __cdecl dbg_SerializePFS(ULinkerLoad_opaque *L)
{
	log_info(">> ULinkerLoad::SerializePackageFileSummary  linker=%p",
	         (void *)L);
	int r = g_orig_SPFS(L);
	log_info("<< ULinkerLoad::SerializePackageFileSummary  linker=%p  -> %d",
	         (void *)L, r);
	return r;
}

static int __cdecl dbg_PreLoadObjects(FAsyncPackage_opaque *p)
{
	log_info(">> FAsyncPackage::PreLoadObjects  pkg=%p", (void *)p);
	int r = g_orig_PLO(p);
	log_info("<< FAsyncPackage::PreLoadObjects  pkg=%p  -> %d", (void *)p, r);
	return r;
}

static int __cdecl dbg_FAsyncTick(FAsyncPackage_opaque *p, int ul, float tl)
{
	log_info(">> FAsyncPackage::Tick  pkg=%p  use=%d  limit=%.4f", (void *)p,
	         ul, (double)tl);
	int r = g_orig_FAT(p, ul, tl);
	log_info("<< FAsyncPackage::Tick  pkg=%p  -> %d", (void *)p, r);
	return r;
}

namespace debug
{

	bool install_linker_debug_hooks()
	{
		HMODULE mod = GetModuleHandleW(nullptr);
		bool all_ok = true;

		auto maybe_hook = [&](bool enable, const char *pat, const char *label,
		                      void *detour, void **orig) -> bool
		{
			if (!enable)
				return true;
			void *addr = FindPatternString(mod, pat);
			if (addr)
			{
				log_info("debug: %-40s = %p", label, addr);
				hook::add(addr, detour, orig);
				return true;
			}
			log_err("debug: %-40s NOT FOUND — hook skipped", label);
			all_ok = false;
			return false;
		};

		maybe_hook(kEnable_Tick, kPat_Tick, "ULinkerLoad::Tick",
		           (void *)&dbg_Tick, (void **)&g_orig_Tick);

		maybe_hook(kEnable_Create, kPat_Create, "ULinkerLoad::Create",
		           (void *)&dbg_Create, (void **)&g_orig_Create);

		maybe_hook(kEnable_CreateExport, kPat_CreateExport,
		           "ULinkerLoad::CreateExport", (void *)&dbg_CreateExport,
		           (void **)&g_orig_CE);

		maybe_hook(kEnable_SerializePFS, kPat_SerializePackageFileSummary,
		           "ULinkerLoad::SerializePackageFileSummary",
		           (void *)&dbg_SerializePFS, (void **)&g_orig_SPFS);

		maybe_hook(kEnable_PreLoadObjs, kPat_PreLoadObjects,
		           "FAsyncPackage::PreLoadObjects", (void *)&dbg_PreLoadObjects,
		           (void **)&g_orig_PLO);

		maybe_hook(kEnable_FAsyncTick, kPat_FAsyncTick, "FAsyncPackage::Tick",
		           (void *)&dbg_FAsyncTick, (void **)&g_orig_FAT);

		log_info("debug: install_linker_debug_hooks done (%s)",
		         all_ok ? "all queued" : "some skipped");
		return all_ok;
	}

}  // namespace debug
