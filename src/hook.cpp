#define WIN32_LEAN_AND_MEAN
#include "hook.hpp"
#include "linker_hook.hpp"
#include "logs.hpp"
#include <windows.h>

static PatternHookEntry g_pattern_hooks[] = {
    HOOK(StaticFindObjectFast),
};

namespace hook {

void install_all() {
	HMODULE exe = GetModuleHandleW(nullptr);

	int ok = 0, fail = 0;
	for (auto &e : g_pattern_hooks) {
		log_info("hook: [%s] scanning...", e.name);
		void *addr = FindPatternString(exe, e.pattern);
		if (!addr) {
			log_err("hook: [%s] pattern not found", e.name);
			++fail;
			continue;
		}
		if (trampoline_install(e.hook, addr, e.detour)) {
			log_info("hook: [%s] installed at %p", e.name, addr);
			++ok;
		} else {
			log_err("hook: [%s] trampoline_install failed at %p", e.name, addr);
			++fail;
		}
	}
	log_info("hook: done — ok=%d fail=%d", ok, fail);
}

void remove_all() {
	for (auto &e : g_pattern_hooks)
		if (e.hook->installed)
			trampoline_remove(e.hook);

	linker_hook::remove_vtable();
	log_info("hook: all hooks removed");
}

} // namespace hook
