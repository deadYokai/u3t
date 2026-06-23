#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// FArchive virtual-function slot indices
// Verified for this game binary (MSVC x64, single scalar dtor at slot 0):
//   kVT_Serialize=1 confirmed, kFNameSlot=6 confirmed (linker_layout.cpp),
//   kVT_Seek=13, kVT_Precache=16, kVT_GetError=21 all confirmed via
//   cross-check. Source: UnArc.h virtual function declaration order.
// ---------------------------------------------------------------------------
namespace FArchiveVtSlots
{
	static constexpr int Dtor = 0;       // ~FArchive
	static constexpr int Serialize = 1;  // Serialize(void*, INT)
	static constexpr int SerializeBits = 2;
	static constexpr int SerializeInt = 3;
	static constexpr int Preload = 4;
	static constexpr int CountBytes = 5;
	static constexpr int SerializeName = 6;    // operator<<(FName&)
	static constexpr int SerializeObject = 7;  // operator<<(UObject*&)
	static constexpr int GetArchiveName = 8;
	static constexpr int GetLinker = 9;
	static constexpr int Tell = 10;       // Tell()
	static constexpr int TotalSize = 11;  // TotalSize()
	static constexpr int AtEnd = 12;
	static constexpr int Seek = 13;  // Seek(INT)
	static constexpr int AttachBulkData = 14;
	static constexpr int DetachBulkData = 15;
	static constexpr int Precache = 16;  // Precache(INT, INT)
	static constexpr int FlushCache = 17;
	static constexpr int SetCompressionMap = 18;
	static constexpr int Flush = 19;
	static constexpr int Close = 20;
	static constexpr int GetError = 21;  // GetError() / IsError fallback
	static constexpr int TotalSlots = 32;
}  // namespace FArchiveVtSlots

// ---------------------------------------------------------------------------
// FArchiveFields — mirror of FArchive's protected data block (UnArc.h)
// x64: vtable ptr(8) immediately precedes this at the object head.
// All UE3 UBOOL/INT fields are 4 bytes; the block is 32 × 4 = 128 bytes,
// tightly packed (no padding: all fields are the same alignment).
// ---------------------------------------------------------------------------
struct FArchiveFields
{
	int32_t ArVer;                              // offset  +8 from obj base
	int32_t ArNetVer;                           // +12
	int32_t ArLicenseeVer;                      // +16
	int32_t ArIsLoading;                        // +20
	int32_t ArIsSaving;                         // +24
	int32_t ArIsTransacting;                    // +28
	int32_t ArWantBinaryPropertySerialization;  // +32
	int32_t ArForceUnicode;                     // +36
	int32_t ArIsPersistent;                     // +40
	int32_t ArForEdit;                          // +44
	int32_t ArForClient;                        // +48
	int32_t ArForServer;                        // +52
	int32_t ArIsError;                          // +56
	int32_t ArIsCriticalError;                  // +60
	int32_t ArContainsCookedData;               // +64
	int32_t ArContainsCode;                     // +68
	int32_t ArContainsMap;                      // +72
	int32_t ArForceByteSwapping;                // +76
	int32_t ArSerializingDefaults;              // +80
	int32_t ArIgnoreArchetypeRef;               // +84
	int32_t ArIgnoreOuterRef;                   // +88
	int32_t ArIgnoreClassRef;                   // +92
	int32_t ArAllowEliminatingReferences;       // +96
	int32_t ArAllowLazyLoading;                 // +100
	int32_t ArIsObjectReferenceCollector;       // +104
	int32_t ArIsCountingMemory;                 // +108
	uint32_t ArPortFlags;                       // +112
	int32_t ArShouldSkipBulkData;               // +116
	int32_t ArIsSaveGame;                       // +120
	int32_t ArIsFinalPackageSave;               // +124
	int32_t ArMaxSerializeSize;                 // +128
	int32_t ArIsFilterEditorOnly;               // +132
	// 32 fields × 4 bytes = 128 bytes
};

static_assert(sizeof(FArchiveFields) == 128, "FArchiveFields layout wrong");

// ---------------------------------------------------------------------------
// UE3MemoryReader
// A fake FArchive that reads from a caller-supplied memory buffer.
// Memory layout: vtable ptr (8) | FArchiveFields (128) | reader state (24)
// The first 136 bytes are identical to a real FArchive object, so the
// engine's FORCEINLINE accessors (IsLoading, Ver, LicenseeVer, etc.) that
// read directly from the object work correctly.
// ---------------------------------------------------------------------------
struct UE3MemoryReader
{
	// --- FArchive-compatible header (must stay first) ---
	void *vptr;         // offset  0 : vtable pointer
	FArchiveFields ar;  // offset  8 : all FArchive data fields (128 bytes)
	// total FArchive footprint = 136 bytes

	// --- reader state (engine never touches these) ---
	const uint8_t *buf;  // offset 136
	size_t pos;          // offset 144
	size_t size;         // offset 152

	static void *g_vtable[FArchiveVtSlots::TotalSlots];
	static bool g_vtable_built;

	static void build_vtable();

	// version  = ArVer  (package file version, e.g. 801)
	// lver     = ArLicenseeVer
	// netver   = ArNetVer (0 = inherit from ArVer)
	UE3MemoryReader(const uint8_t *data, size_t sz, int32_t version,
	                int32_t lver = 0, int32_t netver = 0);
};
