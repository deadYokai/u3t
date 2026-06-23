#pragma once
#include <cstddef>
#include <cstdint>

struct LinkerLayout
{
	ptrdiff_t farchive_off = 0;  // FArchive subobj offset from linker base
	int fname_slot = -1;         // FArchive vtable slot for operator<<(FName&)
	ptrdiff_t nm_data_off = 0;   // NameMap.Data offset from linker base
	ptrdiff_t nm_count_off = 0;  // NameMap.Num  offset from linker base

	// Scanned from Preload bytecode:
	//   uobj_serialize_vtslot  — vtable slot index (×8 = byte offset) for
	//                            UObject::Serialize(FArchive&).  -1 = not
	//                            found.
	//   g_serialized_obj       — pointer to the GSerializedObject global.
	//                            nullptr = not found (skip write, safe to
	//                            omit).
	int uobj_serialize_vtslot = -1;
	void **g_serialized_obj = nullptr;

	bool valid = false;
};

// Resolves layout constants.  preload_fn is the address of
// ULinkerLoad::Preload — used to scan for Serialize vtable slot and
// GSerializedObject.  Pass nullptr to skip those scans.
bool resolve_linker_layout(LinkerLayout &out, void *fname_init = nullptr,
                           void *preload_fn = nullptr);

const LinkerLayout &linker_layout();
