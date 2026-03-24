#pragma once
#define WIN32_LEAN_AND_MEAN
#include <string>
#include <windows.h>

std::wstring get_exe_dir();
std::wstring to_wide(const std::string &s);
std::string to_narrow(const std::wstring &w);
