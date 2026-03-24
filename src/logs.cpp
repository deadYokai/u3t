#define WIN32_LEAN_AND_MEAN
#include "logs.hpp"
#include "util.hpp"
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <windows.h>

namespace logs {

struct Logger {
	std::ofstream file;
	std::mutex mtx;
	DWORD start_tick = 0;
};

static Logger *g_logger = nullptr;

void init(const std::wstring &exe_dir) {
	SYSTEMTIME st{};
	GetLocalTime(&st);

	std::string dir = to_narrow(exe_dir);
	char name[MAX_PATH * 4]{};
	snprintf(name, sizeof(name), "%s\\dinput8.log", dir.c_str());

	auto *lg = new Logger();
	lg->file.open(name, std::ios::trunc | std::ios::out);
	if (!lg->file.is_open()) {
		delete lg;
		return;
	}
	lg->start_tick = GetTickCount();
	g_logger = lg;

	char hdr[512];
	raw_write(hdr);
}

void raw_write(const char *s) {
	if (!g_logger || !g_logger->file.is_open())
		return;
	std::lock_guard<std::mutex> lk(g_logger->mtx);
	g_logger->file << s;
	g_logger->file.flush();
}

void write_line(const char *level, const char *msg) {
	if (!g_logger || !g_logger->file.is_open())
		return;
	char line[4096];
	snprintf(line, sizeof(line), "[%s] %s\n", level, msg);
	std::lock_guard<std::mutex> lk(g_logger->mtx);
	g_logger->file << line;
	g_logger->file.flush();
}

std::string vfmt(const char *fmt, ...) {
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}

} // namespace log
