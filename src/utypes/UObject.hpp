#pragma once
#include "FName.hpp"
#include <cstddef>
#include <cstdint>

struct UObject_Mirror;
struct UClass_Mirror;
struct ULinkerLoad_Mirror;
struct FStateFrame_Mirror;

#pragma pack(push, 4)

struct UObject_Mirror
{
	void *_vfptr;                      // 0x00  vtable pointer

	int32_t Index;                     // 0x08  GObjObjects slot (AddObject)
	uint64_t ObjectFlags;              // 0x0C  EObjectFlags (QWORD)

	UObject_Mirror *HashNext;          // 0x14  name-hash chain
	UObject_Mirror *HashOuterNext;     // 0x1C  outer-hash chain

	FStateFrame_Mirror *StateFrame;    // 0x24
	ULinkerLoad_Mirror *_Linker;       // 0x2C

	int32_t _LinkerIndex;              // 0x34  index into linker ExportMap
	int32_t NetIndex;                  // 0x38

	UObject_Mirror *Outer;             // 0x3C
	FName Name;                        // 0x44  FName{Index@0x44, Number@0x48}

	UClass_Mirror *Class;              // 0x4C
	UObject_Mirror *ObjectArchetype;   // 0x54
};

#pragma pack(pop)

static_assert(offsetof(UObject_Mirror, Index) == 0x08);
static_assert(offsetof(UObject_Mirror, ObjectFlags) == 0x0C);
static_assert(offsetof(UObject_Mirror, HashNext) == 0x14);
static_assert(offsetof(UObject_Mirror, HashOuterNext) == 0x1C);
static_assert(offsetof(UObject_Mirror, StateFrame) == 0x24);
static_assert(offsetof(UObject_Mirror, _Linker) == 0x2C);
static_assert(offsetof(UObject_Mirror, _LinkerIndex) == 0x34);
static_assert(offsetof(UObject_Mirror, NetIndex) == 0x38);
static_assert(offsetof(UObject_Mirror, Outer) == 0x3C);
static_assert(offsetof(UObject_Mirror, Name) == 0x44);
static_assert(offsetof(UObject_Mirror, Class) == 0x4C);
static_assert(offsetof(UObject_Mirror, ObjectArchetype) == 0x54);
static_assert(sizeof(UObject_Mirror) >= 0x5C);

inline const UObject_Mirror *as_uobject(const void *p)
{
	return static_cast<const UObject_Mirror *>(p);
}

inline UObject_Mirror *as_uobject(void *p)
{
	return static_cast<UObject_Mirror *>(p);
}
