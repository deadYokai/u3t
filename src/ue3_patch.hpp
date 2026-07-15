#pragma once
#include <cstdint>

struct UE3FName
{
	int32_t Index;
	int32_t Number;
};

//   x86: Data@0 Num@4 Max@8 (size 0xC)   x64: Data@0 Num@8 Max@0xC (0x10)
struct UE3TArray
{
	void *Data;
	int32_t Num;
	int32_t Max;
};

int ue3_append_names(void *linker, const UE3FName *names, int count);

int ue3_append_name_strings(void *linker, const wchar_t *const *names,
                            int count);
