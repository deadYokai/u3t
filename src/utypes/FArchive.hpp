

#pragma once
#include <cstdint>

using INT = int32_t;
using UINT = uint32_t;
using UBOOL = int32_t;
using BYTE = uint8_t;

namespace FArchiveVtSlot
{

	static constexpr int Dtor = 0;
	static constexpr int Serialize = 1;
	static constexpr int SerializeBits = 2;
	static constexpr int SerializeInt = 3;
	static constexpr int Preload = 4;
	static constexpr int CountBytes = 5;
	static constexpr int SerializeFName = 6;
	static constexpr int SerializeUObject = 7;
	static constexpr int GetArchiveName = 8;
	static constexpr int GetLinker = 9;
	static constexpr int Tell = 10;
	static constexpr int TotalSize = 11;
	static constexpr int AtEnd = 12;
	static constexpr int Seek = 13;
	static constexpr int AttachBulkData = 14;
	static constexpr int DetachBulkData = 15;
	static constexpr int Precache = 16;
	static constexpr int FlushCache = 17;
	static constexpr int SetCompressionMap = 18;
	static constexpr int Flush = 19;
	static constexpr int Close = 20;
	static constexpr int GetError = 21;
	static constexpr int MarkScriptSerializationStart = 22;
	static constexpr int MarkScriptSerializationEnd = 23;
	static constexpr int WillSerializePotentialCrossLevel = 24;
	static constexpr int IsCloseComplete = 25;
	static constexpr int IsFilterEditorOnly = 26;
	static constexpr int SetFilterEditorOnly = 27;
	static constexpr int Count = 28;
}  // namespace FArchiveVtSlot

#pragma pack(push, 4)

struct FArchiveData
{

	INT ArVer;
	INT ArNetVer;
	INT ArLicenseeVer;
	UBOOL ArIsLoading;
	UBOOL ArIsSaving;
	UBOOL ArIsTransacting;
	UBOOL ArWantBinaryPropertySerialization;
	UBOOL ArForceUnicode;
	UBOOL ArIsPersistent;
	UBOOL ArForEdit;
	UBOOL ArForClient;
	UBOOL ArForServer;
	UBOOL ArIsError;
	UBOOL ArIsCriticalError;
	UBOOL ArContainsCookedData;
	UBOOL ArContainsCode;
	UBOOL ArContainsMap;
	UBOOL ArForceByteSwapping;
	INT ArSerializingDefaults;
	UBOOL ArIgnoreArchetypeRef;
	UBOOL ArIgnoreOuterRef;
	UBOOL ArIgnoreClassRef;
	INT ArAllowEliminatingReferences;
	UBOOL ArAllowLazyLoading;
	UBOOL ArIsObjectReferenceCollector;
	UBOOL ArIsCountingMemory;
	DWORD ArPortFlags;
	UBOOL ArShouldSkipBulkData;
	UBOOL ArIsSaveGame;
	UBOOL ArIsFinalPackageSave;
	INT ArMaxSerializeSize;
	UBOOL ArIsFilterEditorOnly;
};

#pragma pack(pop)
static_assert(sizeof(FArchiveData) == 128, "FArchiveData must be 128 bytes");
static_assert(offsetof(FArchiveData, ArVer) == 0x00);
static_assert(offsetof(FArchiveData, ArIsLoading) == 0x0C);
static_assert(offsetof(FArchiveData, ArForceByteSwapping) == 0x44);
static_assert(offsetof(FArchiveData, ArIsFilterEditorOnly) == 0x7C);
