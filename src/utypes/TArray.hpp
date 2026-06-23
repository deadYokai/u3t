

#pragma once
#include <cstddef>
#include <cstdint>

#pragma pack(push, 8)

template <typename T> struct TArray
{
	T *Data;
	int32_t Num;
	int32_t Max;

	T *begin() { return Data; }

	const T *begin() const { return Data; }

	T *end() { return Data + Num; }

	const T *end() const { return Data + Num; }

	T &operator[](int32_t i) { return Data[i]; }

	const T &operator[](int32_t i) const { return Data[i]; }

	bool valid_index(int32_t i) const { return i >= 0 && i < Num; }
};

#pragma pack(pop)
static_assert(sizeof(TArray<void *>) == 16);
static_assert(offsetof(TArray<void *>, Data) == 0);
static_assert(offsetof(TArray<void *>, Num) == 8);
static_assert(offsetof(TArray<void *>, Max) == 12);
