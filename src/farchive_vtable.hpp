#pragma once

#include <cstddef>

struct FArchiveSlots
{
	int Serialize = -1;      // Serialize(void*, INT)
	int SerializeName = -1;  // operator<<(FName&)
	int Tell = -1;           // Tell()
	int Seek = -1;           // Seek(INT)
	int TotalSize = -1;      // TotalSize()
	int Precache = -1;       // Precache(INT, INT)
	int GetError = -1;       // GetError() / IsError
	int total = 0;           // vtable slot count to copy for the BufReader
	bool validated = false;  // anchors matched the standard layout
};

bool resolve_farchive_slots(FArchiveSlots &out, void *preload, void *fname_op,
                            void *seek_impl, ptrdiff_t farchive_off,
                            ptrdiff_t loader_off);
