#pragma once
#define WIN32_LEAN_AND_MEAN
#include <cstdint>
#include <initializer_list>
#include <string>
#include <windows.h>

#include <Zydis/Zydis.h>

std::wstring get_exe_dir();
std::wstring get_mods_dir();
std::wstring to_wide(const std::string &s);
std::string to_narrow(const std::wstring &w);

bool is_mnemonic(const ZydisDecodedInstruction &in,
                 std::initializer_list<ZydisMnemonic> mnemonics);

bool is_flow_break(const ZydisDecodedInstruction &in);

bool is_reg(const ZydisDecodedOperand &op);
bool is_reg(const ZydisDecodedOperand &op, ZydisRegister r);
bool is_imm(const ZydisDecodedOperand &op);
bool is_imm(const ZydisDecodedOperand &op, uint64_t v);
bool is_mem(const ZydisDecodedOperand &op);

bool is_mem_reg(const ZydisDecodedOperand &op);
bool is_mem_rip(const ZydisDecodedOperand &op);

struct AsmAgo
{
	int w;
	int n;

	explicit AsmAgo(int window) : w(window), n(window + 1) {}

	bool seen() const { return n <= w; }

	void hit() { n = 0; }

	void tick()
	{
		if (n <= w)
			++n;
	}

	void reset() { n = w + 1; }
};
