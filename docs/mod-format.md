# Mod Format

A mod is a folder containing a `mod.toml` manifest plus an `overrides/`
directory of binary export overrides. `mod_loader::discover()` scans the
`Mods/` directory at startup and parses each `mod.toml` it finds into a
`ModConfig`.

## `mod.toml`

```toml
name = "ExampleMod"
version = "1.0.0"
author = "deadYokai"
description = "Replaces a UI font"

# enabled, content_path, spawn_patches, replace_patches
# are all optional fields
```

| Field | Required | Notes |
|---|---|---|
| `name` | yes | Display name, shown in the boot log |
| `version` | yes | Free-form string |
| `author` | yes | Free-form string |
| `description` | yes | Free-form string |
| `enabled` | no | Defaults to on; set `false` to disable without removing the folder |
| `content_path` | no | Redirects a package's content path, resolved by `mod_loader::register_content()` |
| `spawn_patches` | no | See below |
| `replace_patches` | no | See below |

### `spawn_patches`

Each entry is a `SpawnPatch`:

```toml
[[spawn_patches]]
class_name = "..."
package_name = "..."
object_name = "..."
```

### `replace_patches`

Each entry is a `ReplacePatch` — a find/replace rule resolved by
`mod_loader::find_replace()` against content paths the game requests:

```toml
[[replace_patches]]
original = "..."
replacement = "..."
```

## `overrides/`

Binary export overrides go in a matching subfolder. The folder name and
file names are keyed to the UE3 export the mod is replacing:

```text
overrides/
├── Dishonored_MainMenu/
│   ├── Dishonored_MainMenu.namemap
│   ├── …MainMenu_IA2.bin
│   └── …MainMenu_IE9.bin
└── UI_Loading_SF_LOC_INT/
    ├── …ChaletComprime.bin
    ├── …PageA.bin
    └── …LOC_INT.namemap
```

- **`.bin`** — the raw serialized export payload that replaces the
  original bytes at `Preload` time.
- **`.namemap`** — a remap table translating the mod's local `FName`
  indices into the target linker's name table, so the override's names
  resolve correctly even if the two builds' name tables don't line up.

At runtime, `override_loader::discover()` indexes every `.bin`/`.namemap`
pair it finds under a mod's `overrides/` folder into an `OverrideRecord`
(key, binary payload, remap table). When the game calls `Preload` on a
matching export, `override_loader::find()` looks up the record by key and
the hook substitutes the override's bytes for the original serialized
data before the object deserializes.

!!! note
    Overrides are matched by key at `Preload` time, in memory. The
    original `.upk` on disk is never opened for writing.
