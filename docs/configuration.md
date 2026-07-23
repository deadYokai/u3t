# Configuration

Everything CU3ML reads and writes lives in the game's `Mods/` directory —
two levels above the executable. Three files matter:

| File | Written by | Purpose |
|---|---|---|
| `cu3ml.toml` | loader / manager | Loader settings and per-mod enable state |
| `cu3ml.addrlist` | loader | Cached engine addresses for this exact `.exe` |
| `cu3ml.log` | loader | Log from the most recent launch |

## `cu3ml.toml`

Written on first launch with defaults, and rewritten whenever something
changes. Safe to hand-edit while the game is closed.

```toml
# CU3ML - crappy unreal engine 3 mod loader
# Written by the loader. Safe to edit while the game is closed.

[loader]
# embedded Lua VM and mod scripts (-nolua overrides this)
lua = true
# verbose Lua logging (-luaverbose overrides this)
lua_verbose = false
# open the manager on every launch, without -cu3ml-manager
manager_at_start = false

[mods]
# key = mod folder name inside this directory
"ExampleMod" = true
"SomeOtherMod" = false
```

`[loader]` accepts `true/false`, `1/0`, `yes/no`, `on/off`; unknown keys are
logged as warnings and ignored.

`[mods]` keys are **folder names**, matched case-insensitively. On every
launch the loader rewrites this table to exactly the set of mod folders it
found: new folders are added as enabled, folders that no longer exist are
dropped. Editing a value to `false` keeps the mod on disk but excludes it
from content registration, replace patches, override discovery and Lua
startup scripts.

## Command-line switches

Pass these to the game's executable (via a shortcut, launch options, or your
launcher of choice). Both `-switch` and `/switch` work, case-insensitively.

| Switch | Effect |
|---|---|
| `-cu3ml-manager` | Halt at the start of `GEngineLoop::Init` and open the [manager](manager.md) |
| `-cu3ml-noaddrcache` | Ignore `cu3ml.addrlist` and force a full re-scan (the fresh results are still written back) |
| `-nolua` | Disable the Lua VM for this launch, overriding `lua = true` |
| `-luaverbose` | Log the result of every chunk the Lua host runs |
| `-lua=<path>` | Run an extra Lua script after all mod scripts. Repeatable |

Any other `-switch` on the command line is recorded and visible to scripts
via `ue3.has_switch("switch")`.

## `cu3ml.addrlist`

A flat `key value` text file of everything the resolver worked out by
disassembly — pointers stored as RVAs from the module base, scalars as
struct offsets and vtable slots.

```text
# cu3ml address cache - generated automatically.
# Pointers are RVAs from the main module base; scalars are
# struct offsets / vtable slots. Delete this file (or pass
# -cu3ml-noaddrcache) to force a full re-scan.
cache.version 3
cache.bits 64
image.timestamp 0x...
image.size 0x...
image.checksum 0x...
image.entry 0x...
engine.EngineLoopInit 0x...
lua.Localize 0x...
```

The header pins the cache to one specific executable: cache format version,
pointer size, and the PE timestamp, size, checksum and entry point. If any
of those don't match, the whole file is discarded and the loader re-scans.
An RVA that falls outside the image is rejected rather than dereferenced,
and subsystems can call `addr_cache::invalidate()` at runtime — the Lua host
does this if hooks built from cached addresses fail to install.

Deleting the file is always safe; the next launch just takes longer.

## `cu3ml.log`

Recreated on every launch (the previous run's log is not kept). It records
the resolved layout, every discovered mod and override, each hook
installation, and all output from scripts. It's the first thing to
attach to a bug report — see [FAQ](faq.md).
