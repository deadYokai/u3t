cmake_minimum_required(VERSION 3.25)
project(ue3_wrapper LANGUAGES C CXX)

if(NOT WIN32 AND NOT CMAKE_CROSSCOMPILING)
    message(
        FATAL_ERROR
        "dinput8 wrapper must be cross-compiled for Windows. "
        "Pass -DCMAKE_TOOLCHAIN_FILE=<mingw-toolchain> or run via build.rs."
    )
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ZYDIS_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_MAN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_ENCODER ON CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_DECODER ON CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_AVX512 ON CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_KNC ON CACHE BOOL "" FORCE)

set(SOL2_TESTS OFF CACHE BOOL "" FORCE)
set(SOL2_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SOL2_DOCS OFF CACHE BOOL "" FORCE)

add_subdirectory(third_party/zydis EXCLUDE_FROM_ALL)

if(MINGW)
    foreach(_t Zydis Zycore)
        if(TARGET ${_t})
            target_compile_options(
                ${_t}
                PRIVATE -fno-stack-protector -U_FORTIFY_SOURCE
            )
        endif()
    endforeach()
endif()

file(GLOB WRAPPER_SOURCES CONFIGURE_DEPENDS src/*.cpp src/manager/*.cpp)

file(GLOB LUA_SRC CONFIGURE_DEPENDS third_party/lua/l*.c)
list(
    REMOVE_ITEM LUA_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua/lua.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua/luac.c
)

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)


add_library(dinput8 SHARED ${IMGUI_SOURCES} ${LUA_SRC} ${WRAPPER_SOURCES})

target_include_directories(
    dinput8
    PRIVATE
        src
		${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        third_party/zydis/include
        third_party/zydis/dependencies/zycore/include
        third_party/sol2/include
        third_party/lua
)

target_compile_definitions(dinput8 PRIVATE SOL_LUA_VERSION=504 IMGUI_IMPL_WIN32_DISABLE_GAMEPAD UNICODE _UNICODE)

target_link_libraries(dinput8 PRIVATE Zydis psapi opengl32 gdi32 dwmapi imm32)

set_target_properties(dinput8 PROPERTIES PREFIX "")
if(MSVC)
    target_link_options(
        dinput8
        PRIVATE "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/dinput8.def" ""
    )
else()
    target_link_options(
        dinput8
        PRIVATE "-Wl,--kill-at" "-Wl,${CMAKE_CURRENT_SOURCE_DIR}/dinput8.def"
    )
endif()

if(MINGW)
    target_compile_options(
        dinput8
        PRIVATE -fno-stack-protector -U_FORTIFY_SOURCE -Wa,-mbig-obj
    )
    target_link_options(dinput8 PRIVATE -static-libgcc -static-libstdc++)
endif()

install(TARGETS dinput8 RUNTIME DESTINATION .)
