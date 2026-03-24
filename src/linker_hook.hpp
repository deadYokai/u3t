#pragma once
#include "trampoline_hook.hpp"
#include "ue3_types.hpp"

extern TrampolineHook Hook_StaticFindObjectFast;

void *__fastcall Hooked_StaticFindObjectFast(void *cls, void *outer, FName name,
                                             int bExact, int bAny,
                                             uint64_t excl);

namespace linker_hook {
void ensure_vtable_hook();
void remove_vtable();
} // namespace linker_hook
