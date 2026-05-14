#define WIN32_LEAN_AND_MEAN
#include "logs.hpp"
#include "pattern_scanner.hpp"
#include "ue3_types.hpp"
#include <cstring>
#include <psapi.h>
#include <windows.h>

static const char *kPat_StaticFindObjectFast =
#ifdef _WIN64
    "48 89 5c 24 08 48 89 74 24 10 4c 89 44 24 18 57 48 83 ec 30 83 3d e1 ff "
    "74 01 00 41 8b d9 48 8b fa 48 8b f1";
#else
    "55 8B EC 51 53 8B 5D ? 56 8B 75 ?";
#endif

static const char *kPat_StaticLoadObject =
#ifdef _WIN64
    "48 8b c4 4c 89 48 20 48 89 50 10 48 89 48 08 53 56 57 48 81 ec f0 00 00 "
    "00";
#else
    "55 8B EC 83 EC ? 53 56 57 8B 7D ?";
#endif

static const char *kPat_StaticConstructObject =
#ifdef _WIN64
    "48 8b c4 4c 89 40 18 57 41 54 41 55 48 83 ec 70 48 c7 44 24 50 fe ff ff "
    "ff 48 89 58 08 48 89 68 10 48 89 70 20 4d 8b e1 48 8b da 48 8b f1 48 8b "
    "bc 24 c8 00 00 00";
#else
    "55 8B EC 83 EC ? 53 56 57 FF 75 ?";
#endif

static const char *kPat_CreatePackage =
#ifdef _WIN64
    "48 8b c4 48 89 48 08 53 55 56 57 41 54";
#else
    "55 8B EC 8B 45 08 85 C0 75 ?";
#endif

static const char *kPat_FNameInit =
#ifdef _WIN64
    "40 55 56 57 41 56 48 81 EC C8 0C 00 00";
#else
    "55 8B EC 83 EC 18 53 56 8B 75 08 85 F6";
#endif

static const char *kPat_GetPackageLinker =
#ifdef _WIN64
    "40 53 56 57 41 54 41 55 41 56 41 57 48 81 ec d0 02 00 00 48 c7 84 24 b8 "
    "00 00 00 fe ff ff ff 48 8b 05 ?? ?? ?? ??";
#else
    "55 8B EC 83 EC ? 53 56 57 8B 75 08";
#endif

#ifdef _WIN64
static constexpr size_t kStrOffCandidates[] = {16, 20, 24};
#else
static constexpr size_t kStrOffCandidates[] = {8, 12, 16};
#endif

static bool validate_fnames(void *cand, bool &out_wf, size_t &out_str_off)
{
	auto *a = static_cast<FNameNamesArray *>(cand);
	if (!a->Data || a->Num < 100 || a->Max < a->Num)
		return false;
	if (IsBadReadPtr(a->Data, sizeof(void *) * 4))
		return false;

	void *e0 = a->Data[0];
	if (!e0 || IsBadReadPtr(e0, 64))
		return false;

	for (size_t off : kStrOffCandidates)
	{
		const char *s =
		    reinterpret_cast<const char *>(static_cast<uint8_t *>(e0) + off);
		if (IsBadReadPtr(s, 4))
			continue;
		if ((s[0] == 'N' || s[0] == 'n') && s[1] == 'o' && s[2] == 'n' &&
		    s[3] == 'e')
		{
#ifdef _WIN64
			out_wf = (off != 16);
#else
			out_wf = (off != 8);
#endif
			out_str_off = off;
			return true;
		}
	}
	return false;
}

static void *scan_rip_mov(const uint8_t *start, size_t len)
{
	for (size_t i = 0; i + 7 <= len; ++i)
	{
		if ((start[i] & 0xF8) != 0x48)
			continue;
		if (start[i + 1] != 0x8B)
			continue;
		if ((start[i + 2] & 0xC7) != 0x05)
			continue;
		int32_t disp;
		memcpy(&disp, start + i + 3, 4);
		return const_cast<uint8_t *>(start) + i + 7 + disp;
	}
	return nullptr;
}

static FNameNamesArray *scan_body_for_fnames(const uint8_t *body, size_t len,
                                             bool &out_wf, size_t &out_str_off)
{
	for (size_t i = 0; i + 7 <= len; ++i)
	{
		if ((body[i] & 0xF8) != 0x48)
			continue;
		if (body[i + 1] != 0x8B)
			continue;
		if ((body[i + 2] & 0xC7) != 0x05)
			continue;
		int32_t disp;
		memcpy(&disp, body + i + 3, 4);
		void *cand = const_cast<uint8_t *>(body) + i + 7 + disp;
		if (validate_fnames(cand, out_wf, out_str_off))
			return static_cast<FNameNamesArray *>(cand);
	}
	return nullptr;
}

static FNameNamesArray *find_fname_names(void *fname_init, bool &out_wf,
                                         size_t &out_str_off)
{
	const auto *fn = static_cast<const uint8_t *>(fname_init);

	for (size_t i = 0; i + 5 <= 0x80; ++i)
	{
		if (fn[i] != 0xE8)
			continue;
		int32_t rel;
		memcpy(&rel, fn + i + 1, 4);
		const uint8_t *sub = fn + i + 5 + rel;
		if (auto *f = scan_body_for_fnames(sub, 0x800, out_wf, out_str_off))
		{
			log_info("ue3_resolve: FNameNames via FNameInit sub-call %p  "
			         "(str_off=%zu  with_flags=%d)",
			         sub, out_str_off, (int)out_wf);
			return f;
		}
		break;
	}

	if (auto *f = scan_body_for_fnames(fn, 0x600, out_wf, out_str_off))
	{
		log_info("ue3_resolve: FNameNames via FNameInit body  "
		         "(str_off=%zu  with_flags=%d)",
		         out_str_off, (int)out_wf);
		return f;
	}

	return nullptr;
}

static UE3Addrs g_ue3;

UE3Addrs &ue3() { return g_ue3; }

bool ue3_resolve(UE3Addrs &out)
{
	HMODULE mod = GetModuleHandleW(nullptr);

	auto scan = [&](const char *pat, const char *name) -> void *
	{
		void *p = FindPatternString(mod, pat);
		if (p)
			log_info("ue3_resolve: %-28s = %p", name, p);
		else
			log_err("ue3_resolve: %-28s NOT FOUND", name);
		return p;
	};

	out.FNameInit = scan(kPat_FNameInit, "FNameInit");
	out.StaticFindObjectFast =
	    scan(kPat_StaticFindObjectFast, "StaticFindObjectFast");
	out.StaticLoadObject = scan(kPat_StaticLoadObject, "StaticLoadObject");
	out.StaticConstructObject =
	    scan(kPat_StaticConstructObject, "StaticConstructObject");
	out.CreatePackage = scan(kPat_CreatePackage, "CreatePackage");

	auto *gpl =
	    static_cast<uint8_t *>(FindPatternString(mod, kPat_GetPackageLinker));
	if (gpl)
	{
		out.GPackageFileCache = static_cast<void **>(scan_rip_mov(gpl, 0x300));
		if (out.GPackageFileCache)
			log_info("ue3_resolve: %-28s = %p", "GPackageFileCache",
			         out.GPackageFileCache);
		else
			log_warn(
			    "ue3_resolve: GPackageFileCache RIP-MOV not found in body");
	}
	else
	{
		log_warn("ue3_resolve: GetPackageLinker not found — "
		         "GPackageFileCache unavailable");
	}

	bool wf = true;
	size_t str_off = kStrOffCandidates[1];
	if (out.FNameInit)
	{
		out.FNameNames = find_fname_names(out.FNameInit, wf, str_off);
	}
	else
	{
		log_warn("ue3_resolve: FNameInit missing, skipping FNameNames scan");
	}
	out.name_layout.with_flags = wf;
	out.name_layout.str_off = str_off;

	if (out.FNameNames)
		log_info("ue3_resolve: %-28s = %p  (Num=%d  str_off=%zu  layout=%s)",
		         "FNameNames", out.FNameNames, out.FNameNames->Num, str_off,
		         wf ? "with_flags" : "no_flags");
	else
		log_warn("ue3_resolve: FNameNames not found — name logging silent");

	if (!out.StaticFindObjectFast || !out.StaticLoadObject)
	{
		log_err("ue3_resolve: critical functions not found, aborting");
		return false;
	}
	if (!out.StaticConstructObject)
		log_warn("ue3_resolve: StaticConstructObject missing — spawn disabled");
	if (!out.CreatePackage)
		log_warn("ue3_resolve: CreatePackage missing — spawn disabled");

	return true;
}
