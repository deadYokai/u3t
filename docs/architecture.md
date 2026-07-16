# Architecture

## Why disassemble instead of hardcoding offsets

UE3 games ship with wildly different compiler settings and struct padding
build to build. Instead of maintaining an offset table per game, the
resolver reads the machine code around known anchor functions and
extracts what it needs live — the same displacement work a human would do
by hand in a disassembler.

## Boot sequence

`dllmain → loaded mod`

1. **Hijack the load order** — `dllmain.cpp`
   The game asks Windows for `dinput8.dll` and gets this one.
   `DirectInput8Create` is re-exported and forwards transparently once the
   real system DLL (or a local `dinput8.orig.dll`, if present) has loaded.
   A background thread also hooks `EngineLoopInit` so initialization runs
   once the engine itself is far enough along to be safely inspected.

2. **Discover mods** — `mod_loader::discover()`
   Scans `Mods/` for folders with a `mod.toml`, parses it into a
   `ModConfig`, and builds the active mod list.

3. **Re-derive the engine layout** — `ue3_resolve()`
   No hardcoded offsets. Anchor functions are found by the string
   constants they reference, then the surrounding x86/x64 is walked with
   a Zydis-backed disassembler to recover struct offsets, vtable slots,
   and array bases from the live binary.

4. **Install the hooks** — `hook::install_all()`
   Patches `StaticLoadObject` to redirect mod-declared content paths, and
   `Preload` so export overrides can be swapped in before an object
   deserializes.

5. **Serve overrides** — `override_loader::`
   When the game preloads a matching export, the loader substitutes the
   mod's `.bin` payload — remapped through its `.namemap` — for the
   original serialized bytes.

## Module breakdown

`src/` — 13 translation units, ~5k lines.

| Module | Responsibility |
|---|---|
| `dllmain` | DirectInput8 proxy; hooks `EngineLoopInit`; orchestrates every subsystem's init order from a background thread |
| `anchor` | Locates functions by the string literals they reference, then walks their cross-references (`ModuleImage`, `find_wstr_all`, `find_cstr_all`) |
| `disp_extract` | Generic Zydis-backed decode primitives — `first_neg_lea`, `first_imul_imm`, `rip_store_then_vcall`, `nth_rip_global`, `first_rip_global_noncookie` (namespace `dx`) |
| `disp_extract_arch` | UE3-specific decode queries built on the primitives above — `serialized_object_and_serialize`, `gpackagefilecache`, `field_off_for_vslot`, `indexed_store_global` (namespace `dxa`) |
| `ue3_resolve` | Ties `anchor` + `disp_extract`/`disp_extract_arch` together into one resolved layout |
| `ue3_layout` | The resolved layout itself — `FArchiveFields` (ArVer, ArIsLoading, ArIsSaving, ArIsTransacting, …), linker and `FName` table offsets |
| `ue3_patch` | Appends `FName` entries into a live linker (`ue3_append_names`, `ue3_append_name_strings`); defines `UE3FName`/`UE3TArray` layouts for both x86 and x64 |
| `farchive_vtable` | Resolves and validates `FArchive` vtable slots — `Serialize`, `SerializeName`, `Tell`, `Seek`, `TotalSize`, `Precache`, `GetError` — into an `FArchiveSlots` struct with a `validated` flag |
| `hook` | Minimal inline hook manager; batches detours and commits them together once resolution succeeds |
| `mod_loader` | Reads `mod.toml` manifests, registers content-path redirects, resolves find/replace rules |
| `override_loader` | Indexes each mod's binary export overrides and name-maps into `OverrideRecord`s, then serves them from the `Preload` hook |
| `config` | TOML parsing into `ModConfig`, `SpawnPatch`, `ReplacePatch` |
| `logs` | Rotating file logger — `log_info` / `log_warn` / `log_err` |
| `util` | `get_exe_dir()`, wide/narrow string conversion helpers |

!!! note "`ue3_layout` is header-only"
    `ue3_layout.hpp` defines the resolved-layout structs but has no
    corresponding `.cpp` — it's populated by `ue3_resolve`, not a
    standalone unit itself.

## The disassembly primitives, by name

The lower-level `dx`/`dxa` functions are named after the instruction
pattern they look for, not what they're used for — a `nth_rip_global`
call in one place might resolve `GPackageFileCache` and in another resolve
an `FName` table base. The named patterns currently implemented:

`first_neg_lea` · `first_imul_imm` · `rip_store_then_vcall` ·
`nth_rip_global` · `indexed_store_global` · `field_disp_before_vcall` ·
`imm_then_call`

Each one is a small, targeted scan over a bounded instruction window —
not a general-purpose decompiler. `ue3_resolve()` calls them in sequence
against known anchor functions and cross-checks the results before
`farchive_vtable::resolve_farchive_slots()` marks the layout `validated`.
