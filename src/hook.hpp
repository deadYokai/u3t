#pragma once
#include "pattern_scanner.hpp"
#include "trampoline_hook.hpp"
#include <windows.h>

struct PatternHookEntry {
	const char *name;
	const char *pattern;
	TrampolineHook *hook;
	void *detour;
};

#define HOOK(X)                                                                \
	PatternHookEntry {                                                         \
		#X, PATTERN_##X, &Hook_##X, reinterpret_cast<void *>(&Hooked_##X)      \
	}

namespace hook {
void install_all();
void remove_all();
} // namespace hook
