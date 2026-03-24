#define WIN32_LEAN_AND_MEAN
#include "util.hpp"
#include <array>
#include <windows.h>

std::wstring get_exe_dir() {
	wchar_t buf[32768]{};
	DWORD len = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
	if (!len)
		return {};
	std::wstring path(buf, len);
	auto pos = path.find_last_of(L"/\\");
	return (pos != std::wstring::npos) ? path.substr(0, pos) : path;
}

std::wstring to_wide(const std::string &s) {
	if (s.empty())
		return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
	if (!w.empty() && w.back() == L'\0')
		w.pop_back();
	return w;
}

std::string to_narrow(const std::wstring &w) {
	if (w.empty())
		return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr,
	                            nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr,
	                    nullptr);
	if (!s.empty() && s.back() == '\0')
		s.pop_back();
	return s;
}
