# Building from Source

Most people don't need this — grab a build from the downloads page instead.
This is for building `dinput8.dll` yourself.

## Requirements

- CMake 3.25+, targeting Windows (native MSVC or a MinGW cross toolchain)
- [Zydis](https://github.com/zyantific/zydis), bundled as a static
  subdirectory dependency for the disassembler
- Links against `psapi` for module enumeration

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
the expected proxy name Windows looks for. Drop it next to the game's
`.exe`.

## Project layout

```text
src/
├── dllmain.cpp
├── anchor.cpp / .hpp
├── disp_extract.cpp / .hpp
├── disp_extract_arch.cpp / .hpp
├── ue3_resolve.cpp
├── ue3_layout.hpp
├── ue3_patch.cpp / .hpp
├── farchive_vtable.cpp / .hpp
├── hook.cpp / .hpp
├── mod_loader.cpp / .hpp
├── override_loader.cpp / .hpp
├── config.cpp / .hpp
├── logs.cpp / .hpp
└── util.cpp / .hpp
```

See [Architecture](architecture.md) for what each module is responsible
for.
