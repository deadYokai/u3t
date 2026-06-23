

#pragma once
#include "FName.hpp"
#include <cstdint>

struct UObject_Mirror;
struct UClass_Mirror;
struct ULinkerLoad_Mirror;
struct FStateFrame_Mirror;

struct UObject_Mirror
{
	void *_vfptr;

	int32_t Index;
	uint64_t ObjectFlags;

	UObject_Mirror *HashNext;
	UObject_Mirror *HashOuterNext;

	FStateFrame_Mirror *StateFrame;
	ULinkerLoad_Mirror *_Linker;

	intptr_t _LinkerIndex;

	int32_t NetIndex;

	UObject_Mirror *Outer;
	FName Name;

	UClass_Mirror *Class;
	UObject_Mirror *ObjectArchetype;
};

inline const UObject_Mirror *as_uobject(const void *p)
{
	return static_cast<const UObject_Mirror *>(p);
}

inline UObject_Mirror *as_uobject(void *p)
{
	return static_cast<UObject_Mirror *>(p);
}
