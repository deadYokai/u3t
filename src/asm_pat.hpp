#pragma once
#include <Zydis/Zydis.h>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "util.hpp"

namespace asmpat
{
	enum class OpKind
	{
		AnyReg,
		AnyMem,
		AnyImm,
		ExactReg,
		ExactImm,
		ExactMemBase,
		ExactMemDisp
	};

	struct OperandSpec
	{
		OpKind kind;
		ZydisRegister reg = ZYDIS_REGISTER_NONE;
		uint64_t imm = 0;
	};

	struct InsnPattern
	{
		ZydisMnemonic mnemonic = ZYDIS_MNEMONIC_INVALID;
		std::vector<OperandSpec> operands;
		std::string src;
	};

	InsnPattern parse(const std::string &pattern);

	bool op_matches(const OperandSpec &spec, const ZydisDecodedOperand &op);
	bool matches(const InsnPattern &pat, const ZydisDecodedInstruction &in,
	             const ZydisDecodedOperand *ops);

	enum class OnDecodeFail
	{
		Stop,
		SkipByte
	};

	struct Watch
	{
		InsnPattern pat;
		AsmAgo ago;
	};

	class AsmFindPat
	{
	  public:
		AsmFindPat(std::initializer_list<std::string> watch,
		           std::initializer_list<std::string> breaks,
		           const std::string &target, int window,
		           OnDecodeFail on_fail = OnDecodeFail::Stop);

		template <typename DecodeFn>
		const uint8_t *scan(const uint8_t *begin, const uint8_t *end,
		                    DecodeFn &&decode)
		{
			for (auto &w : watches_)
				w.ago.reset();

			const uint8_t *p = begin;
			ZydisDecodedInstruction in;
			ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];

			while (p < end)
			{
				if (!decode(p, end, in, ops))
				{
					if (on_fail_ == OnDecodeFail::SkipByte)
					{
						++p;
						for (auto &w : watches_)
							w.ago.reset();
						continue;
					}
					break;
				}

				if (matches(target_, in, ops) && all_seen())
					return p;

				bool broke = false;
				for (auto &b : breaks_)
					if (matches(b, in, ops))
					{
						broke = true;
						break;
					}
				if (broke)
				{
					for (auto &w : watches_)
						w.ago.reset();
					p += in.length;
					continue;
				}

				for (auto &w : watches_)
					w.ago.tick();

				for (auto &w : watches_)
					if (matches(w.pat, in, ops))
						w.ago.hit();

				p += in.length;
			}
			return nullptr;
		}

	  private:
		bool all_seen() const;

		int window_;
		OnDecodeFail on_fail_;
		InsnPattern target_;
		std::vector<Watch> watches_;
		std::vector<InsnPattern> breaks_;
	};

	const uint8_t *asmfindpat(const void *begin, const void *end,
	                          std::initializer_list<std::string> watch,
	                          std::initializer_list<std::string> breaks,
	                          const std::string &target, int window,
	                          OnDecodeFail on_fail = OnDecodeFail::Stop);
}  // namespace asmpat
