# Building from Source

Most people don't need this — grab a build from the downloads page instead.
This is for building `dinput8.dll` yourself.

## Requirements

- CMake 3.25+, targeting Windows (native MSVC or a MinGW cross toolchain).
  The build refuses to configure for a non-Windows target.
- A C++17 compiler.
- The vendored dependencies, all git submodules under `third_party/`:

| Submodule | Used for |
|---|---|
| [zydis](https://github.com/zyantific/zydis) | Instruction decoding for the runtime resolver; built as a static subdirectory dependency |
| [lua](https://github.com/lua/lua) | Lua 5.4, compiled straight from `l*.c` (minus `lua.c` / `luac.c`) |
| [sol2](https://github.com/ThePhD/sol2) | Header-only C++ bindings for Lua |
| [imgui](https://github.com/ocornut/imgui) | Manager UI, with the Win32 and OpenGL 3 backends |

```bash
git clone --recurse-submodules <repo>
# or, in an existing clone:
git submodule update --init --recursive
```

System libraries linked: `psapi`, `opengl32`, `gdi32`, `dwmapi`, `imm32`.

## Build

=== "Native MSVC"

    ```bash
    cmake -B build
    cmake --build build --config Release
    ```

=== "MinGW cross-compile"

    ```bash
    cmake -B build -DCMAKE_TOOLCHAIN_FILE=<mingw>.cmake
    cmake --build build
    ```

Either way, the output is `build/dinput8.dll` — no `lib` prefix, matching
the expected proxy name Windows looks for. Exports come from `dinput8.def`
(`/DEF:` under MSVC, `--kill-at` plus the def file under MinGW). Drop the
result next to the game's `.exe`.

Build x86 and x64 separately, into separate build directories — the address
cache format is pinned to pointer size, and a mismatched DLL simply won't
load into the game.

## Project layout

```text
src/
├── dllmain.cpp                  proxy + boot orchestration
├── mod_loader.cpp / .hpp        mod.toml discovery, ordering, SLO redirects
├── override_loader.cpp / .hpp   .bin / .namemap / .newexports
├── config.cpp / .hpp            mod.toml parser
├── loader_config.cpp / .hpp     Mods/cu3ml.toml
├── addr_cache.cpp / .hpp        Mods/cu3ml.addrlist
├── lua_host.cpp / .hpp          Lua 5.4 VM and the `ue3` bindings
├── ue3_api.cpp / .hpp           lazy engine-fn wrappers, output capture
├── ue3_resolve.cpp              layout resolution
├── ue3_layout.hpp               resolved layout structs (header-only)
├── ue3_patch.cpp / .hpp         live linker name/export appends
├── farchive_vtable.cpp / .hpp   FArchive vtable slot recovery
├── anchor.cpp / .hpp            string-anchored function location
├── disp_extract.cpp / .hpp      generic decode primitives (dx)
├── disp_extract_arch.cpp / .hpp UE3-specific decode queries (dxa)
├── asm_pat.cpp / .hpp           textual instruction-pattern matcher
├── hook.cpp / .hpp              inline hook manager
├── logs.cpp / .hpp              Mods/cu3ml.log
├── util.cpp / .hpp              paths, string conversion, Zydis predicates
└── manager/
    ├── ui.cpp / .hpp            manager window
    ├── gl_ctx.cpp / .hpp        Win32 + OpenGL 3 context
    ├── theme.cpp / .hpp         colors, fonts, widgets
    └── crt_post.cpp / .hpp      CRT post-processing pass
```

Sources are globbed (`src/*.cpp`, `src/manager/*.cpp`) with
`CONFIGURE_DEPENDS`, so adding a file doesn't need a `CMakeLists.txt` edit —
but does need a re-configure on some generators.

See [Architecture](architecture.md) for what each module is responsible
for.
