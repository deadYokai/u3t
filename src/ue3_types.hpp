#pragma once
#include "pattern_scanner.hpp"
#include <cstdint>
#include <cstring>
#include <string>
#include <windows.h>

struct FName {
	int32_t Index = 0;
	int32_t Number = 0;
};
static_assert(sizeof(FName) == 8);

template <typename T> struct TArrayView {
	T *Data = nullptr;
	int32_t Num = 0;
	int32_t Max = 0;

	T &operator[](int i) { return Data[i]; }
	const T &operator[](int i) const { return Data[i]; }
	bool valid() const { return Data != nullptr && Num > 0; }
};

struct FStringLayout {
	wchar_t *Data = nullptr;
	int32_t Num = 0;
	int32_t Max = 0;
};
static_assert(sizeof(FStringLayout) == 16);

struct FNameEntry_hdr {
	uint64_t _flags;
	uint32_t EncodedIndex;
};
static constexpr size_t kFNameEntry_StrOff = 0x14;
static constexpr uint32_t kFNameEntry_UnicodeBit = 1u;

struct FNamesArray {
	FNameEntry_hdr **Data;
	int32_t Num;
	int32_t Max;
};

namespace LinkerOff {
static constexpr ptrdiff_t LinkerRoot = 0x005c;
static constexpr ptrdiff_t NameMap = 0x011c;
static constexpr ptrdiff_t Filename = 0x01a4;
static constexpr ptrdiff_t LoadFlags = 0x0250;
static constexpr ptrdiff_t Loader = 0x0668;
static constexpr ptrdiff_t FirstPatchFlag = 0x0698;
static constexpr int NumPatchFlags = 6;
static constexpr int LoadFlagQuiet = 0x2001;
} // namespace LinkerOff

namespace UObjectOff {
static constexpr ptrdiff_t Outer = 0x003c;
static constexpr ptrdiff_t ObjectName = 0x0044;
static constexpr ptrdiff_t Class = 0x004c;
} // namespace UObjectOff

using FNameInit_fn = void (*)(FName *, const wchar_t *, int32_t, int32_t,
                              int32_t);

using StaticFindObjectFast_fn = void *(__fastcall *)(void *, void *, FName, int,
                                                     int, uint64_t);

using StaticLoadObject_fn = void *(__fastcall *)(void *, void *,
                                                 const wchar_t *,
                                                 const wchar_t *, uint32_t,
                                                 void *, int);

using FindPackageFile_fn = int(__fastcall *)(void *, const wchar_t *, void *,
                                             FStringLayout *, const wchar_t *);

// FNameInit @ 0x14005fcf0
#define PATTERN_FNameInit "40 55 56 57 41 56 48 81 EC C8 0C 00 00"

// UObject::StaticFindObjectFast @ 0x140091dd0
#define PATTERN_StaticFindObjectFast                                           \
	"48 89 5C 24 08 48 89 74 24 10 4C 89 44 24 18 57 48 83 EC 30 83 3D"

// UObject::StaticLoadObject @ 0x14010e0a0
#define PATTERN_StaticLoadObject                                               \
	"48 8B C4 4C 89 48 20 48 89 50 10 48 89 48 08 53 56 57 48 81 EC F0 00"

#define PATTERN_GetPackageLinker                                               \
	"40 53 56 57 41 54 41 55 41 56 41 57 48 81 ec d0 02 00 00 48 c7 84 24 "    \
	"b8 00 00 00 fe ff ff ff 48 8b 05 ?? ?? ?? ??"

struct UE3Addrs {
	FNameInit_fn FNameInit = nullptr;
	StaticFindObjectFast_fn StaticFindObjectFast = nullptr;
	StaticLoadObject_fn StaticLoadObject = nullptr;
	void **GPackageFileCache = nullptr;
	FNamesArray *FNameNames = nullptr;
};

extern UE3Addrs g_ue3;
inline UE3Addrs &ue3() { return g_ue3; }

inline bool ue3_resolve(UE3Addrs &out) {
	HMODULE exe = GetModuleHandleW(nullptr);

	out.FNameInit = reinterpret_cast<FNameInit_fn>(
	    FindPatternString(exe, PATTERN_FNameInit));

	out.StaticFindObjectFast = reinterpret_cast<StaticFindObjectFast_fn>(
	    FindPatternString(exe, PATTERN_StaticFindObjectFast));

	out.StaticLoadObject = reinterpret_cast<StaticLoadObject_fn>(
	    FindPatternString(exe, PATTERN_StaticLoadObject));

	{
		auto *gpl = static_cast<uint8_t *>(
		    FindPatternString(exe, PATTERN_GetPackageLinker));
		if (gpl) {
			for (size_t i = 0; i + 7 <= 0x300; ++i) {
				if (gpl[i] == 0x48 && gpl[i + 1] == 0x8B &&
				    gpl[i + 2] == 0x0D) {
					int32_t disp;
					memcpy(&disp, gpl + i + 3, 4);
					out.GPackageFileCache =
					    reinterpret_cast<void **>(gpl + i + 7 + disp);
					break;
				}
			}
		}
	}

	if (out.FNameInit) {
		auto scan_for_names = [](const uint8_t *start,
		                         size_t len) -> FNamesArray * {
			for (size_t i = 0; i + 7 <= len; ++i) {
				if ((start[i] & 0xF8) != 0x48)
					continue;
				if (start[i + 1] != 0x8B)
					continue;
				if ((start[i + 2] & 0xC7) != 0x05)
					continue;

				int32_t disp;
				memcpy(&disp, start + i + 3, 4);
				auto *cand = reinterpret_cast<FNamesArray *>(
				    const_cast<uint8_t *>(start) + i + 7 + disp);

				if (!cand->Data || cand->Num < 100 || cand->Max < cand->Num)
					continue;

				FNameEntry_hdr *e0 = cand->Data[0];
				if (!e0)
					continue;
				const char *s0 = reinterpret_cast<const char *>(
				    reinterpret_cast<const uint8_t *>(e0) + kFNameEntry_StrOff);
				if ((s0[0] != 'N' && s0[0] != 'n') || s0[1] != 'o' ||
				    s0[2] != 'n' || s0[3] != 'e')
					continue;

				return cand;
			}
			return nullptr;
		};

		const auto *fn = reinterpret_cast<const uint8_t *>(out.FNameInit);

		const uint8_t *init_body = nullptr;
		for (size_t i = 0; i + 5 <= 0x80; ++i) {
			if (fn[i] == 0xE8) {
				int32_t rel;
				memcpy(&rel, fn + i + 1, 4);
				init_body = fn + i + 5 + rel;
				break;
			}
		}

		if (init_body)
			out.FNameNames = scan_for_names(init_body, 0x800);
		if (!out.FNameNames)
			out.FNameNames = scan_for_names(fn, 0x600);
	}

	return out.FNameInit && out.StaticFindObjectFast && out.StaticLoadObject &&
	       out.GPackageFileCache;
}

inline FName fname_find(const wchar_t *str) {
	FName out{};
	if (ue3().FNameInit)
		ue3().FNameInit(&out, str, 0, 0, 0);
	return out;
}

inline FName fname_create(const wchar_t *str) {
	FName out{};
	if (ue3().FNameInit)
		ue3().FNameInit(&out, str, 0, 1, 0);
	return out;
}

inline std::string fname_str(int32_t idx) {
	const FNamesArray *arr = ue3().FNameNames;
	if (!arr || idx < 0 || idx >= arr->Num)
		return {};
	const FNameEntry_hdr *e = arr->Data[idx];
	if (!e)
		return {};
	const auto *raw = reinterpret_cast<const uint8_t *>(e) + kFNameEntry_StrOff;
	if (e->EncodedIndex & kFNameEntry_UnicodeBit) {
		const wchar_t *ws = reinterpret_cast<const wchar_t *>(raw);
		std::string s;
		for (; *ws; ++ws)
			s += (*ws < 128) ? static_cast<char>(*ws) : '?';
		return s;
	}
	return std::string(reinterpret_cast<const char *>(raw));
}

inline std::string fname_to_string(FName n) {
	std::string s = fname_str(n.Index);
	if (s.empty())
		return {};
	if (n.Number > 0) {
		s += '_';
		s += std::to_string(n.Number - 1);
	}
	return s;
}

inline TArrayView<FName> linker_namemap(void *linker) {
	auto *b = static_cast<uint8_t *>(linker);
	TArrayView<FName> v;
	memcpy(&v.Data, b + LinkerOff::NameMap, 8);
	memcpy(&v.Num, b + LinkerOff::NameMap + 8, 4);
	return v;
}

inline std::string linker_pkg_stem(void *linker) {
	auto *b = static_cast<uint8_t *>(linker);
	wchar_t *wptr = nullptr;
	int32_t wnum = 0;
	memcpy(&wptr, b + LinkerOff::Filename, 8);
	memcpy(&wnum, b + LinkerOff::Filename + 8, 4);
	if (!wptr || wnum <= 1)
		return {};
	std::wstring ws(wptr, wptr + (wnum - 1));
	auto slash = ws.find_last_of(L"\\/");
	if (slash != std::wstring::npos)
		ws = ws.substr(slash + 1);
	auto dot = ws.rfind(L'.');
	if (dot != std::wstring::npos)
		ws.resize(dot);
	std::string out;
	out.reserve(ws.size());
	for (wchar_t c : ws)
		out += static_cast<char>(towlower(c));
	return out;
}

inline void linker_set_all_patch_flags(void *linker) {
	auto *b = static_cast<uint8_t *>(linker);
	for (int i = 0; i < LinkerOff::NumPatchFlags; ++i) {
		int32_t one = 1;
		memcpy(b + LinkerOff::FirstPatchFlag + i * 4, &one, 4);
	}
}
