# Architecture

## Why disassemble instead of hardcoding offsets

UE3 games ship with wildly different compiler settings and struct padding
build to build. Instead of maintaining an offset table per game, the
resolver reads the machine code around known anchor functions and
extracts what it needs live — the same displacement work a human would do
by hand in a disassembler. What it works out is then cached per-executable
in `Mods/cu3ml.addrlist`, so the expensive scan happens once.

## Boot sequence

`dllmain → loaded mod`

1. **Hijack the load order** — `dllmain.cpp`
   The game asks Windows for `dinput8.dll` and gets this one.
   `DirectInput8Create` is re-exported and forwards transparently once the
   real system DLL (or a local `dinput8.orig.dll`, if present) has loaded.

2. **Background init thread** — `init_thread`
   Starts the log, loads the address cache and `cu3ml.toml`, parses Lua
   command-line switches, loads the real `dinput8`, discovers mods, then
   hooks `GEngineLoop::Init` so the rest of initialization runs once the
   engine is far enough along to be safely inspected. The hook target comes
   from the address cache if valid, otherwise from an anchor scan for the
   only function referencing `NoTextureStreaming`.

3. **Discover mods** — `mod_loader::discover()`
   Scans `Mods/` for folders with a `mod.toml`, parses each into a
   `ModConfig`, applies enable state from `cu3ml.toml`, topologically sorts
   by `[dependencies].requires`, and rewrites the config's `[mods]` table to
   match what's on disk.

4. **(Optional) manager** — `ui::run()`
   If `-cu3ml-manager` or `manager_at_start` is set, boot halts inside the
   `GEngineLoop::Init` hook and hands control to the ImGui manager. Quitting
   removes all hooks and exits; launching refreshes the active mod set and
   resumes. See [Manager](manager.md).

5. **Re-derive the engine layout** — `ue3_resolve()`
   Loads from `cu3ml.addrlist` when the cache matches this executable.
   Otherwise: anchor functions are found by the string constants they
   reference, then the surrounding x86/x64 is walked with a Zydis-backed
   disassembler to recover struct offsets, vtable slots and array bases from
   the live binary — and the results are written back to the cache.

6. **Register content and install hooks**
   `mod_loader::register_content()` pushes each mod's `content_path`
   directories into `GPackageFileCache` through its vtable;
   `mod_loader::install_hooks()` queues the `StaticLoadObject` redirect;
   `override_loader::discover()` + `install_hooks()` index every mod's
   `overrides/` tree and queue the `Preload` (and, for new exports,
   `GetPackageLinker`) hooks. `hook::install_all()` commits them together.

7. **Serve overrides** — `override_loader::`
   When the game preloads a matching export, the loader substitutes the
   mod's `.bin` payload — remapped through its `.namemap`, with the linker's
   name table temporarily shadowed — for the original serialized bytes. Any
   `.newexports` entries are appended to the package's export map when its
   linker is created.

8. **Start scripting** — `lua_host::init()`
   Creates the Lua 5.4 VM, binds the `ue3` table, and runs each enabled
   mod's `main.lua` in dependency order, then any `-lua=` scripts. See
   [Lua Scripting](lua-api.md).

## Module breakdown

`src/` — 22 translation units, ~10k lines including headers.

### Loader core

| Module | Responsibility |
|---|---|
| `dllmain` | DirectInput8 proxy; command-line parsing; hooks `GEngineLoop::Init`; orchestrates every subsystem's init order from a background thread |
| `mod_loader` | Reads `mod.toml` manifests, applies enable state, topologically sorts by dependency, registers content paths, installs the `StaticLoadObject` redirect |
| `override_loader` | Indexes each mod's `.bin` / `.namemap` / `.newexports` into `OverrideRecord`s, serves them from the `Preload` hook, and appends new exports at linker creation |
| `config` | Hand-rolled TOML subset parser for `mod.toml` → `ModConfig`, `SpawnPatch`, `ReplacePatch` |
| `loader_config` | `Mods/cu3ml.toml` — `LoaderSettings` plus per-mod enable state, with dirty tracking and write-back |
| `addr_cache` | `Mods/cu3ml.addrlist` — per-executable cache of resolved pointers and scalars, pinned to the PE's timestamp/size/checksum/entry |
| `hook` | Minimal inline hook manager; batches detours and commits them together once resolution succeeds |
| `logs` | File logger for `Mods/cu3ml.log` — `log_info` / `log_warn` / `log_err` |
| `util` | `get_exe_dir()`, `get_mods_dir()`, string conversion, and small Zydis operand predicates |

### Engine resolution

| Module | Responsibility |
|---|---|
| `anchor` | Locates functions by the string literals they reference, then walks cross-references, call targets and callers (`ModuleImage`, `find_wstr_all`, `find_cstr_all`, `direct_callers`, `function_end`) |
| `disp_extract` | Generic Zydis-backed decode primitives — `first_neg_lea`, `first_imul_imm`, `rip_store_then_vcall`, `nth_rip_global`, `indexed_store_global`, `field_disp_before_vcall`, `imm_then_call`, `x64_argnum_liveness` (namespace `dx`) |
| `disp_extract_arch` | UE3-specific decode queries built on those primitives — `serialized_object_and_serialize`, `gpackagefilecache`, `field_off_for_vslot`, `load_and_indexed_store_to_global` (namespace `dxa`) |
| `asm_pat` | Textual instruction-pattern matcher (`asmpat::AsmFindPat`) — declare watch, break and target patterns plus a window, and scan a byte range for the first target seen with all watches recently satisfied |
| `ue3_resolve` | Ties `addr_cache` + `anchor` + `disp_extract`/`disp_extract_arch` together into one resolved layout |
| `ue3_layout` | The resolved layout itself — `FArchiveFields` (ArVer, ArIsLoading, ArIsSaving, ArIsTransacting, …), linker and `FName` table offsets |
| `ue3_patch` | Mutates live linkers — `ue3_append_names`, `ue3_append_name_strings`, `ue3_append_exports`; defines `UE3FName`/`UE3TArray` layouts for both x86 and x64 |
| `farchive_vtable` | Resolves and validates `FArchive` vtable slots — `Serialize`, `SerializeName`, `Tell`, `Seek`, `TotalSize`, `Precache`, `GetError` — into an `FArchiveSlots` struct with a `validated` flag |
| `ue3_api` | Lazy engine-function wrappers: `EngineFn<Sig>` resolves by anchor string on first call; `CaptureOutputDevice` fakes an `FOutputDevice` so console output can be captured into a `std::string` |

### Scripting and UI

| Module | Responsibility |
|---|---|
| `lua_host` | Owns the sol2/Lua 5.4 state, binds the `ue3` table, runs mod scripts, and manages the localization / config-section hooks those bindings need |
| `manager/ui` | The manager window: settings toggles, mod list, detail pane, launch/quit |
| `manager/gl_ctx` | Win32 window + OpenGL 3 context creation and message pump |
| `manager/theme` | Colors, fonts, section labels, buttons, glow and edge drawing |
| `manager/crt_post` | Optional CRT-style post-processing pass; falls back to flat rendering if unavailable |

!!! note "`ue3_layout` is header-only"
    `ue3_layout.hpp` defines the resolved-layout structs but has no
    corresponding `.cpp` — it's populated by `ue3_resolve`, not a
    standalone unit itself.

## The disassembly primitives, by name

The lower-level `dx`/`dxa` functions are named after the instruction
pattern they look for, not what they're used for — a `nth_rip_global`
call in one place might resolve `GPackageFileCache` and in another resolve
an `FName` table base.

Each one is a small, targeted scan over a bounded instruction window —
not a general-purpose decompiler. `ue3_resolve()` calls them in sequence
against known anchor functions and cross-checks the results before
`resolve_farchive_slots()` marks the layout `validated`.

`asm_pat` generalizes the same idea: instead of a hand-written scan loop per
pattern, you write the instructions as short strings and let `AsmFindPat`
handle windowing, flow breaks and decode failures.
