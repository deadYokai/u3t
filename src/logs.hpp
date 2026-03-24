#pragma once
#define WIN32_LEAN_AND_MEAN
#include <string>
#include <windows.h>

namespace logs {
void init(const std::wstring &exe_dir);
void raw_write(const char *s);
void write_line(const char *level, const char *msg);
std::string vfmt(const char *fmt, ...);
} // namespace log

#define log_info(...) logs::write_line("INFO ", logs::vfmt(__VA_ARGS__).c_str())
#define log_warn(...) logs::write_line("WARN ", logs::vfmt(__VA_ARGS__).c_str())
#define log_err(...) logs::write_line("ERROR", logs::vfmt(__VA_ARGS__).c_str())
