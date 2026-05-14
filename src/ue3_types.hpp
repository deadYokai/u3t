#pragma once
#define WIN32_LEAN_AND_MEAN
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <windows.h>

struct FNameOnStack
{
	int32_t Index = 0;
	int32_t Number = 0;
};

template <typename T> struct TArrayMirror
{
	T *Data = nullptr;
	int32_t Num = 0;
	int32_t Max = 0;
};

using FNameNamesArray = TArrayMirror<void *>;

struct FNameLayout
{
#ifdef _WIN64
	size_t str_off = 20;
#else
	size_t str_off = 12;
#endif
	bool with_flags = true;

	int32_t raw_index(const void *e) const
	{
		const auto *b = static_cast<const uint8_t *>(e);
		int32_t v;
		memcpy(&v, b + (with_flags ? 8 : 0), sizeof(v));
		return v;
	}

	bool is_unicode(const void *e) const { return (raw_index(e) & 1) != 0; }

	int name_index(const void *e) const { return raw_index(e) >> 1; }

	const char *ansi(const void *e) const
	{
		return reinterpret_cast<const char *>(static_cast<const uint8_t *>(e) +
		                                      str_off);
	}

	const wchar_t *uni(const void *e) const
	{
		return reinterpret_cast<const wchar_t *>(
		    static_cast<const uint8_t *>(e) + str_off);
	}
};

struct UE3Addrs
{
	void *FNameInit = nullptr;
	void *StaticFindObjectFast = nullptr;
	void *StaticLoadObject = nullptr;
	void *StaticConstructObject = nullptr;
	void *CreatePackage = nullptr;
	void **GPackageFileCache = nullptr;

	FNameNamesArray *FNameNames = nullptr;
	FNameLayout name_layout;
};

UE3Addrs &ue3();
bool ue3_resolve(UE3Addrs &out);
