# Mod Format

A mod is a folder inside `Mods/` containing a `mod.toml` manifest, plus any
combination of an `overrides/` directory, a `main.lua` script, and loose
content files. `mod_loader::discover()` scans `Mods/` at startup, parses each
`mod.toml` into a `ModConfig`, and sorts the results into dependency order.

```text
Mods/
└── ExampleMod/              ← the folder name is the mod's key
    ├── mod.toml
    ├── main.lua             ← optional, runs after the layout resolves
    ├── CookedPCConsole/     ← optional, any content_path target
    └── overrides/
        └── UI_Loading_SF_LOC_INT/
            ├── UI_Loading_SF_LOC_INT.namemap
            ├── UI_Loading_SF_LOC_INT.newexports
            └── ChaletComprime.bin
```

!!! note "The folder name matters"
    The mod's *key* — used in `Mods/cu3ml.toml` to remember whether it's
    enabled — is the folder name, not the `name` field. Renaming the folder
    resets its enable state.

## `mod.toml`

The parser is a small hand-written TOML subset: top-level `key = "value"`
pairs, string arrays, and the array-of-tables and tables listed below.
Comments start with `#`.

```toml
name = "ExampleMod"
version = "1.0.0"
author = "deadYokai"
description = "Replaces UI_Loading.ChaletComprime-CologneEighty with a custom font"

# repeat this key once per directory to register with the package cache
content_path = "CookedPCConsole"

[dependencies]
requires = ["SomeOtherMod", "AnotherMod"]

[[patch.replace]]
original = "UI_Loading.ChaletComprime-CologneEighty"
replacement = "MyFonts.MyReplacementFont"

[[patch.spawn]]
class = "Engine.StaticMeshActor"
package = "MyPackage"
name = "MyActor"
```

| Key | Table | Notes |
|---|---|---|
| `name` | top level | Display name, shown in the log and manager. Falls back to the folder name if omitted |
| `version` | top level | Free-form string |
| `author` | top level | Free-form string |
| `description` | top level | Free-form string, shown in the manager's detail pane |
| `content_path` | top level | Directory relative to the mod folder, registered with `GPackageFileCache` so the engine can find loose packages. Repeat the key to add more than one |
| `requires` | `[dependencies]` | Array of *mod names* (the `name` field, not folder names) this mod must load after |
| `original` / `replacement` | `[[patch.replace]]` | Rewrites a `StaticLoadObject` path — see below |
| `class` / `package` / `name` | `[[patch.spawn]]` | Parsed and counted, but not acted on by the current loader |


### `[[patch.replace]]`

Each entry installs a redirect in the `StaticLoadObject` hook. When the game
asks for `original` by path, CU3ML passes `replacement` through to the engine
instead, with a thread-local guard so the substituted load can't re-enter the
hook. Entries missing either field are logged and skipped, and if a mod set
has no replaces at all the hook isn't installed.

### `[dependencies]`

`requires` names other mods that must be initialized first. The loader
topologically sorts everything it discovered; unknown names are warned about
and ignored, and a dependency cycle is reported with the mods involved
appended in discovery order rather than aborting the boot. The resulting
order is what `main.lua` scripts run in.

## `overrides/`

Either `overrides/` or `Overrides/` is accepted. Each *subfolder* is named
after the UE3 package it targets, and holds the files for that package:

| File | Purpose |
|---|---|
| `<Anything>.bin` | Raw serialized export payload. The filename stem is the lookup key matched at `Preload` time |
| `<PackageName>.namemap` | One name per line, mapping the tool's local `FName` indices onto the target linker's name table. Missing names are appended to the live linker |
| `<PackageName>.newexports` | Optional. Declares brand-new exports to append to the package's export map |

At runtime `override_loader::discover()` walks every enabled mod, indexes each
package folder into `OverrideRecord`s (key, payload bytes, name table), and
installs the `Preload` hook. When the game preloads an export whose key
matches, the record's bytes are deserialized in place of the original — the
linker's name map is temporarily shadowed during the read so the mod's names
resolve against the live table.

Without a `.namemap` the loader still serves the payload but logs that `FName`
remapping is disabled, which is almost always wrong for real objects.

### `.newexports`

A tab-separated table with 11 columns and `#` comments. Lines are sorted by
index and appended to the package's `ExportMap`, letting a mod add objects
the shipped package never contained:

| Col | Field |
|---|---|
| 1 | export index |
| 2 | object path |
| 3 | class name |
| 4 | class index |
| 5 | outer index |
| 6 | super index |
| 7 | archetype index |
| 8 | object flags |
| 9 | export flags |
| 10 | serial size |
| 11 | `.bin` filename backing the export |

Malformed rows are logged with a line number and skipped. If the injection
point would leave a gap in the export map, the loader refuses rather than
writing an export the `.bin` can't reference.

!!! note
    Overrides are matched by key at `Preload` time, in memory. The
    original `.upk` on disk is never opened for writing.

## `main.lua`

If a mod folder contains `main.lua`, it runs once after the UE3 layout is
resolved and hooks are committed, in dependency order. The mod's own folder
is prepended to `package.path`, so `require` works for files shipped
alongside it. See [Lua Scripting](lua-api.md).

## Building overrides

The easiest way to produce `.bin` / `.namemap` / `.newexports` sets is the
`pack-mod` command, check [ue3-tools/Building Mods](https://docs.yokai.digital/ue3-tools/building-mods).
