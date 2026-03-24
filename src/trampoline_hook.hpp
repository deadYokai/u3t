#pragma once
#include <cstddef>
#include <cstdint>
#include <windows.h>

static constexpr size_t kJmpStubSize = 14;
static constexpr size_t kMaxStolenSize = 32;

struct TrampolineHook {
	void *target = nullptr;
	void *detour = nullptr;
	void *trampoline = nullptr;
	uint8_t orig[kMaxStolenSize]{};
	size_t stolen = 0;
	bool installed = false;
};

bool trampoline_install(TrampolineHook *hook, void *target, void *detour);
bool trampoline_remove(TrampolineHook *hook);
