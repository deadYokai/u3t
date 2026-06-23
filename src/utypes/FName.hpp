

#pragma once
#include <cstddef>
#include <cstdint>

using NAME_INDEX = int32_t;

#pragma pack(push, 4)

struct FNameSerial
{
	int32_t NameIndex;
	int32_t Number;
};

#pragma pack(pop)
static_assert(sizeof(FNameSerial) == 8);

#pragma pack(push, 4)

struct FName
{
	NAME_INDEX
	Index;
	int32_t Number;
};

#pragma pack(pop)
static_assert(sizeof(FName) == 8);
static_assert(offsetof(FName, Index) == 0);
static_assert(offsetof(FName, Number) == 4);

static constexpr NAME_INDEX kNameUnicodeMask = 0x1;
static constexpr int kNameIndexShift = 1;

inline int fname_raw_index(const FName &n) { return n.Index; }

inline int fname_entry_index(const FName &n)
{
	return n.Index >> kNameIndexShift;
}

inline bool fname_is_unicode(NAME_INDEX raw)
{
	return (raw & kNameUnicodeMask) != 0;
}
