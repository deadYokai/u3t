#include "asm_pat.hpp"
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <unordered_map>

namespace asmpat
{
	namespace
	{
		std::string lower(std::string s)
		{
			std::transform(s.begin(), s.end(), s.begin(),
			               [](unsigned char c) { return std::tolower(c); });
			return s;
		}

		std::string trim(const std::string &s)
		{
			size_t a = s.find_first_not_of(" \t");
			if (a == std::string::npos)
				return "";
			size_t b = s.find_last_not_of(" \t");
			return s.substr(a, b - a + 1);
		}

		const std::unordered_map<std::string, ZydisMnemonic> &mnemonic_table()
		{
			static const auto table = []
			{
				std::unordered_map<std::string, ZydisMnemonic> t;
				for (int i = 0; i <= ZYDIS_MNEMONIC_MAX_VALUE; ++i)
				{
					auto m = static_cast<ZydisMnemonic>(i);
					if (const char *s = ZydisMnemonicGetString(m))
						t.emplace(s, m);
				}
				return t;
			}();
			return table;
		}

		const std::unordered_map<std::string, ZydisRegister> &register_table()
		{
			static const auto table = []
			{
				std::unordered_map<std::string, ZydisRegister> t;
				for (int i = 0; i <= ZYDIS_REGISTER_MAX_VALUE; ++i)
				{
					auto r = static_cast<ZydisRegister>(i);
					if (const char *s = ZydisRegisterGetString(r))
						t.emplace(s, r);
				}
				return t;
			}();
			return table;
		}

		bool parse_uint(const std::string &s, uint64_t &out)
		{
			if (s.empty())
				return false;
			try
			{
				size_t pos = 0;
				out = std::stoull(s, &pos, 0);
				return pos == s.size();
			}
			catch (...)
			{
				return false;
			}
		}

		OperandSpec parse_operand(const std::string &raw)
		{
			std::string tok = trim(raw);
			std::string low = lower(tok);

			if (low == "reg")
				return {OpKind::AnyReg};
			if (low == "mem")
				return {OpKind::AnyMem};
			if (low == "imm")
				return {OpKind::AnyImm};

			if (tok.size() >= 2 && tok.front() == '[' && tok.back() == ']')
			{
				std::string inner = lower(trim(tok.substr(1, tok.size() - 2)));
				auto &mregs = register_table();
				auto mrit = mregs.find(inner);
				if (mrit != mregs.end())
					return {OpKind::ExactMemBase, mrit->second, 0};

				uint64_t disp;
				if (parse_uint(inner, disp))
					return {OpKind::ExactMemDisp, ZYDIS_REGISTER_NONE, disp};

				throw std::invalid_argument(
				    "asmfindpat: unrecognized memory operand '" + inner +
				    "' in '" + raw +
				    "' (expected a register name or a "
				    "bare displacement)");
			}

			auto &regs = register_table();
			auto rit = regs.find(low);
			if (rit != regs.end())
				return {OpKind::ExactReg, rit->second, 0};

			uint64_t v;
			if (parse_uint(tok, v))
				return {OpKind::ExactImm, ZYDIS_REGISTER_NONE, v};

			throw std::invalid_argument("asmfindpat: unrecognized operand '" +
			                            tok + "'");
		}
	}  // namespace

	InsnPattern parse(const std::string &pattern)
	{
		std::string s = trim(pattern);
		if (!s.empty() && s.front() == '!')
			s.erase(s.begin());
		s = trim(s);

		InsnPattern pat;
		pat.src = pattern;

		size_t sp = s.find_first_of(" \t");
		std::string mnem_str =
		    lower(sp == std::string::npos ? s : s.substr(0, sp));
		std::string rest = sp == std::string::npos ? "" : s.substr(sp + 1);

		auto &mnems = mnemonic_table();
		auto it = mnems.find(mnem_str);
		if (it == mnems.end())
			throw std::invalid_argument("asmfindpat: unrecognized mnemonic '" +
			                            mnem_str + "' in pattern '" + pattern +
			                            "'");
		pat.mnemonic = it->second;

		size_t pos = 0;
		while (pos < rest.size())
		{
			size_t comma = rest.find(',', pos);
			std::string tok = (comma == std::string::npos)
			                      ? rest.substr(pos)
			                      : rest.substr(pos, comma - pos);
			tok = trim(tok);
			if (!tok.empty())
				pat.operands.push_back(parse_operand(tok));
			if (comma == std::string::npos)
				break;
			pos = comma + 1;
		}
		return pat;
	}

	bool op_matches(const OperandSpec &spec, const ZydisDecodedOperand &op)
	{
		switch (spec.kind)
		{
			case OpKind::AnyReg:
				return is_reg(op);
			case OpKind::AnyMem:
				return is_mem(op);
			case OpKind::AnyImm:
				return is_imm(op);
			case OpKind::ExactReg:
				return is_reg(op, spec.reg);
			case OpKind::ExactImm:
				return is_imm(op, spec.imm);
			case OpKind::ExactMemBase:
				return is_mem(op) && op.mem.index == ZYDIS_REGISTER_NONE &&
				       op.mem.base == spec.reg;
			case OpKind::ExactMemDisp:
				return is_mem(op) && op.mem.disp.has_displacement &&
				       static_cast<uint64_t>(op.mem.disp.value) == spec.imm;
		}
		return false;
	}

	bool matches(const InsnPattern &pat, const ZydisDecodedInstruction &in,
	             const ZydisDecodedOperand *ops)
	{
		if (in.mnemonic != pat.mnemonic)
			return false;
		if (pat.operands.size() > in.operand_count_visible)
			return false;
		for (size_t i = 0; i < pat.operands.size(); ++i)
			if (!op_matches(pat.operands[i], ops[i]))
				return false;
		return true;
	}

	AsmFindPat::AsmFindPat(std::initializer_list<std::string> watch,
	                       std::initializer_list<std::string> breaks,
	                       const std::string &target, int window,
	                       OnDecodeFail on_fail)
	    : window_(window), on_fail_(on_fail), target_(parse(target))
	{
		for (auto &w : watch)
			watches_.push_back({parse(w), AsmAgo(window)});
		for (auto &b : breaks)
			breaks_.push_back(parse(b));
	}

	bool AsmFindPat::all_seen() const
	{
		for (auto &w : watches_)
			if (!w.ago.seen())
				return false;
		return true;
	}

	const uint8_t *asmfindpat(const void *begin, const void *end,
	                          std::initializer_list<std::string> watch,
	                          std::initializer_list<std::string> breaks,
	                          const std::string &target, int window,
	                          OnDecodeFail on_fail)
	{
		AsmFindPat m(watch, breaks, target, window, on_fail);

		ZydisDecoder dec;
		ZydisDecoderInit(&dec,
		                 sizeof(void *) == 8 ? ZYDIS_MACHINE_MODE_LONG_64
		                                     : ZYDIS_MACHINE_MODE_LEGACY_32,
		                 sizeof(void *) == 8 ? ZYDIS_STACK_WIDTH_64
		                                     : ZYDIS_STACK_WIDTH_32);

		const uint8_t *b = static_cast<const uint8_t *>(begin);
		const uint8_t *e = static_cast<const uint8_t *>(end);
		return m.scan(
		    b, e,
		    [&](const uint8_t *p, const uint8_t *pe,
		        ZydisDecodedInstruction &in, ZydisDecodedOperand *ops)
		    {
			    return ZYAN_SUCCESS(ZydisDecoderDecodeFull(
			        &dec, p, static_cast<ZyanUSize>(pe - p), &in, ops));
		    });
	}
}  // namespace asmpat
