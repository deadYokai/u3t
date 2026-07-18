#include <atomic>
#define WIN32_LEAN_AND_MEAN
#include "logs.hpp"
#include "util.hpp"
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <windows.h>

namespace logs
{
	static HANDLE g_file = INVALID_HANDLE_VALUE;
	static SRWLOCK g_lock = SRWLOCK_INIT;
	static std::atomic<bool> g_initialized{false};

	void init(const std::wstring &exe_dir)
	{
		AcquireSRWLockExclusive(&g_lock);
		if (!g_initialized)
		{
			std::wstring path = exe_dir + L"\\cu3ml.log";
			HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
			                       nullptr, CREATE_ALWAYS,
			                       FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h != INVALID_HANDLE_VALUE)
			{
				g_file = h;
				g_initialized = true;
			}
		}
		ReleaseSRWLockExclusive(&g_lock);
	}

	void raw_write(const char *s)
	{
		if (!g_initialized || !s)
			return;

		AcquireSRWLockExclusive(&g_lock);
		if (g_file != INVALID_HANDLE_VALUE)
		{
			size_t n = strlen(s);
			if (n)
			{
				DWORD wrote = 0;
				WriteFile(g_file, s, static_cast<DWORD>(n), &wrote, nullptr);
			}
		}
		ReleaseSRWLockExclusive(&g_lock);
	}

	void write_line(const char *level, const char *msg)
	{
		char line[4096];
		snprintf(line, sizeof(line), "[%s] %s\n", level, msg);
		raw_write(line);
	}

	static int format_guarded(char *buf, size_t cap, const char *fmt,
	                          va_list ap)
	{
		__try
		{
			return vsnprintf_s(buf, cap, _TRUNCATE, fmt, ap);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			if (cap)
				buf[0] = '\0';
			return -1;
		}
	}

	std::string vfmt(const char *fmt, ...)
	{
		char buf[2048];
		buf[0] = '\0';

		va_list ap;
		va_start(ap, fmt);
		int len = format_guarded(buf, sizeof(buf), fmt, ap);
		va_end(ap);

		(void)len;
		return std::string(buf);
	}

}  // namespace logs
