# Lua Scripting

CU3ML embeds Lua 5.4 (bound with [sol2](https://github.com/ThePhD/sol2)) and
exposes a single global table, `ue3`, that wraps the engine functions the
loader has resolved.

## When scripts run

The VM is created in `lua_host::init()`, at the end of `GEngineLoop::Init`
processing — after the UE3 layout is resolved, content paths are registered,
and every hook is committed. Then, in dependency order, each enabled mod's
`main.lua` is executed, followed by any scripts passed with `-lua=<path>`.

Standard libraries opened: `base`, `package`, `string`, `table`, `math`,
`os`, `io`, `coroutine`. There is no `debug` and no `ffi`.

Before a mod's script runs, its own folder is appended to `package.path` as
both `<mod>/?.lua` and `<mod>/?/init.lua`, so `require "helpers"` resolves
against files shipped with the mod.

`print` is redirected to the log file, tab-joining its arguments and
prefixing them with `[lua]`. There is no console window.

## Enabling and disabling

| Control | Effect |
|---|---|
| `lua = false` in `Mods/cu3ml.toml` | VM never starts |
| `-nolua` | Overrides the config and disables the VM for this launch |
| `lua_verbose = true` / `-luaverbose` | Logs the result of every chunk the host runs |
| `-lua=C:\path\script.lua` | Runs an extra script after all mod scripts; repeatable |

## Example

```lua
-- Runs once, in dependency order, after CU3ML resolves the UE3 layout.

local b = ue3.build()
ue3.log(("main.lua up on %s (%s)"):format(b.arch, b.loader))

for _, m in ipairs(ue3.mods()) do
	print("mod:", m.name, m.version or "?", "by", m.author or "unknown")
end

print("cmdline:", ue3.cmdline())
if ue3.has_switch("example") then
	ue3.log("simple example of cmdline arg")
end

-- FName index 0 is always "None" — a cheap sanity check that the name
-- table resolved correctly
print("fname(0) =", ue3.fname(0))

local obj = ue3.load("EngineMaterials.DefaultDiffuse")
ue3.log("load DefaultDiffuse -> " .. tostring(obj ~= nil))

local text, handled = ue3.exec("obj classes")
if handled and text then
	ue3.log("obj classes returned " .. tostring(#text) .. " bytes")
end
```

## `ue3` reference

### Logging

| Function | Notes |
|---|---|
| `ue3.log(msg)` | Writes an `INFO` line, prefixed `[lua]` |
| `ue3.warn(msg)` | Writes a `WARN` line |
| `ue3.error(msg)` | Writes an `ERROR` line; does **not** raise a Lua error |
| `ue3.msgbox(text [, caption])` | Blocking `MessageBoxW`. Caption defaults to `"CU3ML"` |

### Environment

| Function | Returns |
|---|---|
| `ue3.build()` | `{ arch = "x64" \| "x86", loader = "CU3ML" }` |
| `ue3.cmdline()` | The raw command line the game was launched with |
| `ue3.has_switch(name)` | `true` if `-name` or `/name` was passed. Case-insensitive; loader-consumed switches like `nolua` are not visible here |
| `ue3.mods()` | Array of `{ name, version, author }` for **enabled** mods, in load order |
| `ue3.module_base()` | Base address of the game module as an integer |
| `ue3.layout()` | `{ ok, StaticLoadObject, StaticFindObjectFast, GetPackageLinker, Preload, FNameInit, GPackageFileCache }` — pointers formatted as hex strings, for logging |

### Objects and names

| Function | Notes |
|---|---|
| `ue3.load(path [, outer])` | Calls `StaticLoadObject`. Returns a pointer, or `nil` if the object wasn't found or `StaticLoadObject` was never resolved |
| `ue3.name(str)` | Interns a string via `FName::Init`, returning `{ index, number }`, or `nil` if `FNameInit` is unresolved |
| `ue3.fname(index)` | Reverse lookup: the string for a name-table index, or `nil`. Handles both ANSI and Unicode name entries |
| `ue3.set_engine_free(ptr_or_rva)` | Tells the loader which function to use to free engine-allocated memory. Accepts a pointer or an integer RVA relative to the module base |

### Console commands

```lua
local out, handled = ue3.exec("obj classes")
```

`ue3.exec(cmd)` calls the engine's global `StaticExec` with a capturing
output device, returning the captured text (or `nil`) plus a boolean saying
whether the command was handled. It covers `obj`, `get`, `set`, `listprops`
and friends without needing a `GEngine` pointer.

For input-object commands, pass the object pointer explicitly:

| Function | Notes |
|---|---|
| `ue3.input.exec(self, cmd)` | `UInput::Exec`. Returns `text, handled` |
| `ue3.input.exec_commands(self, cmd)` | `UInput::ExecInputCommands`. Returns captured text |

Both return `nil, false` if `self` is nil or the engine function couldn't be
resolved from its anchor string.

### Localization and config sections

```lua
ue3.section.localize("Menu", "PlayButton", "MyGamePackage", "START")
ue3.section.set("Engine.Engine", "bSmoothFrameRate", "MyGamePackage", "False")
```

| Function | Notes |
|---|---|
| `ue3.section.localize(section, key, package, new_string)` | Overrides what `Localize()` returns for that triple, in memory, for the lifetime of the process |
| `ue3.section.set(section, key, package, value [, file])` | Removes the existing key then adds `value` |
| `ue3.section.add(section, key, package, value [, file])` | Adds without removing — for multi-value keys |
| `ue3.section.remove(section, key, package [, file])` | Removes the key |
| `ue3.config.load_file(gconfig, filename)` | `FConfigCacheIni::LoadFile` against a config-cache pointer you supply |

The `section.*` calls install their engine hooks lazily on first use. If the
target section isn't loaded yet, the operation is queued and applied once it
appears, so scripts don't have to care about config load order. All four
return `false` when the underlying hooks couldn't be installed at all.

!!! note "Resolution is lazy and anchored"
    Engine functions behind `exec`, `input.*`, `config.*` and `section.*`
    are located on first call by searching for a distinctive string literal
    they reference, then cached in `Mods/cu3ml.addrlist` under `lua.*` keys.
    A call that can't resolve its target logs an error and returns a nil-ish
    result rather than crashing.
