#define WIN32_LEAN_AND_MEAN
#include "util.hpp"
#include <array>
#include <windows.h>

std::wstring get_exe_dir()
{
	wchar_t buf[32768]{};
	DWORD len = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
	if (!len)
		return {};
	std::wstring path(buf, len);
	auto pos = path.find_last_of(L"/\\");
	return (pos != std::wstring::npos) ? path.substr(0, pos) : path;
}

std::wstring get_mods_dir()
{
	std::wstring path = get_exe_dir() + L"\\..\\..\\Mods";

	wchar_t buffer[MAX_PATH];

	DWORD len = GetFullPathNameW(path.c_str(), MAX_PATH, buffer, nullptr);

	if (len == 0 || len >= MAX_PATH)
		return L"";

	return std::wstring(buffer, len);
}

std::wstring to_wide(const std::string &s)
{
	if (s.empty())
		return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
	if (!w.empty() && w.back() == L'\0')
		w.pop_back();
	return w;
}

std::string to_narrow(const std::wstring &w)
{
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

bool is_mnemonic(const ZydisDecodedInstruction &in,
                 std::initializer_list<ZydisMnemonic> mnemonics)
{
	for (ZydisMnemonic m : mnemonics)
		if (in.mnemonic == m)
			return true;
	return false;
}

bool is_flow_break(const ZydisDecodedInstruction &in)
{
	return is_mnemonic(
	    in, {ZYDIS_MNEMONIC_CALL, ZYDIS_MNEMONIC_RET, ZYDIS_MNEMONIC_JMP});
}

bool is_reg(const ZydisDecodedOperand &op)
{
	return op.type == ZYDIS_OPERAND_TYPE_REGISTER;
}

bool is_reg(const ZydisDecodedOperand &op, ZydisRegister r)
{
	return is_reg(op) && op.reg.value == r;
}

bool is_imm(const ZydisDecodedOperand &op)
{
	return op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE;
}

bool is_imm(const ZydisDecodedOperand &op, uint64_t v)
{
	return is_imm(op) && op.imm.value.u == v;
}

bool is_mem(const ZydisDecodedOperand &op)
{
	return op.type == ZYDIS_OPERAND_TYPE_MEMORY;
}

bool is_mem_reg(const ZydisDecodedOperand &op)
{
	return is_mem(op) && op.mem.base != ZYDIS_REGISTER_NONE &&
	       op.mem.base != ZYDIS_REGISTER_RIP;
}

bool is_mem_rip(const ZydisDecodedOperand &op)
{
	return is_mem(op) && op.mem.base == ZYDIS_REGISTER_RIP &&
	       op.mem.disp.has_displacement;
}
