#pragma once
#define WIN32_LEAN_AND_MEAN
#include <string>
#include <windows.h>

namespace logs
{
	void init(bool debug);
	void raw_write(const char *s);
	void write_line(const char *level, const char *msg);
	void write_debug(const char *msg);
	std::string vfmt(const char *fmt, ...);
}  // namespace logs

#define log_info(...) \
	logs::write_line("INFO  ", logs::vfmt(__VA_ARGS__).c_str())
#define log_warn(...) \
	logs::write_line("WARN  ", logs::vfmt(__VA_ARGS__).c_str())
#define log_err(...) logs::write_line("ERROR ", logs::vfmt(__VA_ARGS__).c_str())
#define log_engine(...) \
	logs::write_line("ENGINE", logs::vfmt(__VA_ARGS__).c_str())
#define log_fatal(...) \
	logs::write_line("FATAL ", logs::vfmt(__VA_ARGS__).c_str())

#define log_debug(...) logs::write_debug(logs::vfmt(__VA_ARGS__).c_str())
