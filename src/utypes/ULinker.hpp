

#pragma once
#include "FArchive.hpp"
#include "FName.hpp"
#include "TArray.hpp"
#include "UObject.hpp"
#include <cstddef>
#include <cstdint>
#include <string.h>

static constexpr ptrdiff_t kLinker_FArchiveOff = 0x1C4;
static constexpr ptrdiff_t kLinker_Loader = 0x4A4;
static constexpr ptrdiff_t kLinker_OriginalLoader = 0x514;

static constexpr ptrdiff_t kLinker_NameMap_Data = kLinker_FArchiveOff - 0xA8;
static constexpr ptrdiff_t kLinker_NameMap_Num = kLinker_FArchiveOff - 0xA0;
static constexpr ptrdiff_t kLinker_NameMap_Max = kLinker_NameMap_Num + 4;

static_assert(kLinker_NameMap_Data == 0x11C);
static_assert(kLinker_NameMap_Num == 0x124);

static constexpr ptrdiff_t kUObj_HashNext = offsetof(UObject_Mirror, HashNext);
static constexpr ptrdiff_t kUObj_ObjectFlags =
    offsetof(UObject_Mirror, ObjectFlags);
static constexpr ptrdiff_t kUObj_HashOuterNext =
    offsetof(UObject_Mirror, HashOuterNext);
static constexpr ptrdiff_t kUObj_StateFrame =
    offsetof(UObject_Mirror, StateFrame);
static constexpr ptrdiff_t kUObj_Linker = offsetof(UObject_Mirror, _Linker);
static constexpr ptrdiff_t kUObj_LinkerIndex =
    offsetof(UObject_Mirror, _LinkerIndex);
static constexpr ptrdiff_t kUObj_Index = offsetof(UObject_Mirror, Index);
static constexpr ptrdiff_t kUObj_NetIndex = offsetof(UObject_Mirror, NetIndex);
static constexpr ptrdiff_t kUObj_Outer = offsetof(UObject_Mirror, Outer);
static constexpr ptrdiff_t kUObj_Name = offsetof(UObject_Mirror, Name);
static constexpr ptrdiff_t kUObj_Class = offsetof(UObject_Mirror, Class);
static constexpr ptrdiff_t kUObj_ObjectArchetype =
    offsetof(UObject_Mirror, ObjectArchetype);
static constexpr ptrdiff_t kUObj_Size = sizeof(UObject_Mirror);
static constexpr ptrdiff_t kUObj_MinRead =
    offsetof(UObject_Mirror, Name) + sizeof(FName);

inline uint8_t *linker_base(void *linker)
{
	return static_cast<uint8_t *>(linker);
}

inline const uint8_t *linker_base(const void *linker)
{
	return static_cast<const uint8_t *>(linker);
}

inline void *linker_farchive(void *linker)
{
	return linker_base(linker) + kLinker_FArchiveOff;
}

inline void **linker_loader_ptr(void *linker)
{
	return reinterpret_cast<void **>(linker_base(linker) + kLinker_Loader);
}

inline void **linker_original_loader_ptr(void *linker)
{
	return reinterpret_cast<void **>(linker_base(linker) +
	                                 kLinker_OriginalLoader);
}

inline TArray<FName> *linker_namemap(void *linker)
{
	return reinterpret_cast<TArray<FName> *>(linker_base(linker) +
	                                         kLinker_NameMap_Data);
}

inline uint64_t uobj_flags(const void *obj)
{
	return as_uobject(obj)->ObjectFlags;
}

inline void *uobj_outer(const void *obj) { return as_uobject(obj)->Outer; }

inline FName uobj_name(const void *obj) { return as_uobject(obj)->Name; }
