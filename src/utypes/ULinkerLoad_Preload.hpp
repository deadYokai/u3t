

#pragma once
#include "TArray.hpp"
#include "ULinker.hpp"
#include <cstddef>
#include <cstdint>

static constexpr uint64_t kRF_ClassDefaultObject = 0x0000000000000200ULL;
static constexpr uint64_t kRF_NeedLoad = 0x0000020000000000ULL;

static constexpr uint32_t kEF_None = 0x00000000u;
static constexpr uint32_t kEF_ForcedExport = 0x00000001u;
static constexpr uint32_t kEF_ScriptPatcherExport = 0x00000002u;

struct alignas(8) FObjectExport_Mirror
{

	int32_t ObjectName_Index;
	int32_t ObjectName_Number;
	int32_t OuterIndex;

	int32_t ClassIndex;
	int32_t SuperIndex;
	int32_t ArchetypeIndex;

	uint64_t ObjectFlags;
	int32_t SerialSize;
	int32_t SerialOffset;
	int32_t ScriptSerialStart;
	int32_t ScriptSerialEnd;

	void *_Object;
	int32_t _iHashNext;
	uint32_t ExportFlags;

	uint8_t _gen_count[16];

	uint8_t _pkg_guid[16];

	uint32_t PackageFlags;

	uint32_t _pad_end;
};

static_assert(
    sizeof(FObjectExport_Mirror) == 0x68,
    "FObjectExport_Mirror size mismatch — check UE3 source alignment");
static_assert(offsetof(FObjectExport_Mirror, ObjectFlags) == 0x18);
static_assert(offsetof(FObjectExport_Mirror, SerialSize) == 0x20);
static_assert(offsetof(FObjectExport_Mirror, SerialOffset) == 0x24);
static_assert(offsetof(FObjectExport_Mirror, _Object) == 0x30);
static_assert(offsetof(FObjectExport_Mirror, ExportFlags) == 0x3C);
static_assert(offsetof(FObjectExport_Mirror, PackageFlags) == 0x60);

static constexpr ptrdiff_t kTArray_Size = 16;
static constexpr ptrdiff_t kLinker_ImportMap =
    kLinker_NameMap_Data + kTArray_Size;
static constexpr ptrdiff_t kLinker_ExportMap = kLinker_ImportMap + kTArray_Size;

static_assert(kLinker_ExportMap == 0x13C,
              "ExportMap offset derivation changed — re-check NameMap anchor");

inline TArray<FObjectExport_Mirror> *linker_export_map(void *linker)
{
	return reinterpret_cast<TArray<FObjectExport_Mirror> *>(
	    static_cast<uint8_t *>(linker) + kLinker_ExportMap);
}

inline FObjectExport_Mirror *linker_get_export(void *linker, intptr_t idx)
{
	auto *em = linker_export_map(linker);
	if (!em->Data || idx < 0 || idx >= static_cast<intptr_t>(em->Num))
		return nullptr;
	return &em->Data[idx];
}

inline bool uobj_has_flag(const void *obj, uint64_t flag)
{
	return (as_uobject(obj)->ObjectFlags & flag) != 0;
}

inline void uobj_clear_flag(void *obj, uint64_t flag)
{
	as_uobject(obj)->ObjectFlags &= ~flag;
}
