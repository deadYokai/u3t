#define SOL_ALL_SAFETIES_ON 1
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <sol/sol.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

#include "anchor.hpp"
#include "hook.hpp"
#include "logs.hpp"
#include "lua_host.hpp"
#include "mod_loader.hpp"
#include "ue3_api.hpp"
#include "ue3_layout.hpp"
#include "util.hpp"

namespace
{
	sol::state *g_lua = nullptr;
	bool g_enabled = true;
	bool g_verbose = false;
	bool g_cfg_done = false;

	std::wstring g_cmdline;
	std::vector<std::wstring> g_switches;
	std::vector<std::wstring> g_lua_files;

	std::vector<std::wstring> tokenize(const std::wstring &s)
	{
		std::vector<std::wstring> out;
		std::wstring cur;
		bool in_q = false;
		for (wchar_t c : s)
		{
			if (c == L'"')
				in_q = !in_q;
			else if (!in_q && (c == L' ' || c == L'\t'))
			{
				if (!cur.empty())
				{
					out.push_back(cur);
					cur.clear();
				}
			}
			else
				cur += c;
		}
		if (!cur.empty())
			out.push_back(cur);
		return out;
	}

	std::wstring lower(std::wstring s)
	{
		for (auto &c : s)
			c = (wchar_t)towlower(c);
		return s;
	}

	bool run_chunk(const std::string &code, const char *name)
	{
		if (!g_lua)
			return false;
		try
		{
			auto result = g_lua->safe_script(
			    code,
			    [name](lua_State *, sol::protected_function_result pfr)
			    {
				    sol::error err = pfr;
				    log_err("lua: error in %s: %s", name, err.what());
				    return pfr;
			    },
			    name);
			return result.valid();
		}
		catch (const std::exception &e)
		{
			log_err("lua: exception in %s: %s", name, e.what());
			return false;
		}
		catch (...)
		{
			log_err("lua: unknown exception in %s", name);
			return false;
		}
	}

	std::mutex g_loc_mtx;
	std::unordered_map<std::wstring, std::wstring> g_loc_overrides;
	void *g_orig_localize = nullptr;
	void *g_orig_get_section = nullptr;
	void *g_remove_key = nullptr;
	void *g_add = nullptr;

	std::wstring loc_key(const wchar_t *pkg, const wchar_t *sec,
	                     const wchar_t *key)
	{
		std::wstring s;
		auto app = [&](const wchar_t *p)
		{
			for (; p && *p; ++p)
				s += (wchar_t)towlower(*p);
			s += L'\x01';
		};
		app(pkg);
		app(sec);
		app(key);
		return s;
	}

	std::wstring loc_prefix(const wchar_t *pkg, const wchar_t *sec)
	{
		std::wstring s;

		auto app = [&](const wchar_t *p)
		{
			for (; p && *p; ++p)
				s += (wchar_t)towlower(*p);
			s += L'\x01';
		};

		app(pkg);
		app(sec);

		return s;
	}

	struct EngFString
	{
		wchar_t *Data;
		int Num;
		int Max;
	};

	void build_eng_fstring(EngFString *out, const std::wstring &s)
	{
		out->Data = nullptr;
		out->Num = 0;
		out->Max = 0;
		if (s.empty())
			return;
		void *ra = ue3().ArrayRealloc;
		if (!ra)
			return;
		unsigned bytes = (unsigned)((s.size() + 1) * sizeof(wchar_t));
		using Fn = void *(*)(void *, unsigned, unsigned);
		void *data = (sizeof(void *) == 8)
		                 ? reinterpret_cast<Fn>(ra)(nullptr, 0, bytes)
		                 : reinterpret_cast<Fn>(ra)(nullptr, bytes, 8);
		if (!data)
			return;
		memcpy(data, s.c_str(), s.size() * sizeof(wchar_t));
		((wchar_t *)data)[s.size()] = L'\0';
		out->Data = (wchar_t *)data;
		out->Num = (int)(s.size() + 1);
		out->Max = out->Num;
	}

	std::wstring package_from_filename(const wchar_t *filename)
	{
		if (!filename)
			return L"";
		std::wstring result(filename);

		size_t lastSlash = result.find_last_of(L"\\/");
		if (lastSlash != std::wstring::npos)
			result.erase(0, lastSlash + 1);

		size_t lastDot = result.find_last_of(L'.');
		if (lastDot != std::wstring::npos)
			result.erase(lastDot);

		return result;
	}

	using FNameInitFn = void(UE3_THISCALL *)(FNameStack *self,
	                                         const wchar_t *name, int number,
	                                         int findType, int bSplitName);
	using AddFn = EngFString *(UE3_THISCALL *)(void *Sec, EngFString *Key,
	                                           EngFString *Value);
	using RemoveKeyFn = int(UE3_THISCALL *)(void *Sec, EngFString *Key);

	struct PairInitFN
	{
		FNameStack Key;
		EngFString *Value;
	};

	using RemoveKeyFNFn = int(UE3_THISCALL *)(void *Sec, FNameStack Key);
	using AddFNFn = int *(UE3_THISCALL *)(void *Sec, int *outIdx,
	                                      PairInitFN *kv, int *outFlag);

	void *g_key_ctor = nullptr;
	bool g_fname_keyed = false;

	struct SweepInsn
	{
		const uint8_t *p;
		ZydisMnemonic m;
		uint64_t imm;
		const uint8_t *tgt;
		bool has_mem;
		ZydisRegister mem_base;
		int64_t mem_disp;
	};

	static bool sweep(const uint8_t *b, const uint8_t *e,
	                  std::vector<SweepInsn> &out)
	{
		ZydisDecoder dec;
		if (sizeof(void *) == 8)
			ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64,
			                 ZYDIS_STACK_WIDTH_64);
		else
			ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_COMPAT_32,
			                 ZYDIS_STACK_WIDTH_32);

		for (const uint8_t *p = b; p < e;)
		{
			ZydisDecodedInstruction in;
			ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
			if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(
			        &dec, p, (ZyanUSize)(e - p), &in, ops)))
				return false;

			SweepInsn si{p,     in.mnemonic,         0, nullptr,
			             false, ZYDIS_REGISTER_NONE, 0};
			for (ZyanU8 i = 0; i < in.operand_count_visible; ++i)
			{
				if (ops[i].type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
				{
					if (ops[i].imm.is_relative)
					{
						ZyanU64 a = 0;
						ZydisCalcAbsoluteAddress(&in, &ops[i], (ZyanU64)p, &a);
						si.tgt = (const uint8_t *)a;
					}
					else if (!si.imm)
						si.imm = ops[i].imm.value.u;
				}
				else if (ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY &&
				         !si.has_mem)
				{
					si.has_mem = true;
					si.mem_base = ops[i].mem.base;
					si.mem_disp = ops[i].mem.disp.value;
				}
			}
			out.push_back(si);
			p += in.length;
		}
		return true;
	}

	static int last_cmp_idx(const std::vector<SweepInsn> &v, uint64_t imm)
	{
		int found = -1;
		for (int i = 0; i < (int)v.size(); ++i)
			if (v[i].m == ZYDIS_MNEMONIC_CMP && v[i].imm == imm)
				found = i;
		return found;
	}

	static void branch_calls(const std::vector<SweepInsn> &v, int cmp_idx,
	                         std::vector<std::pair<int, const uint8_t *>> &out)
	{
		const uint8_t *stop = nullptr;
		for (int i = cmp_idx + 1; i < (int)v.size(); ++i)
		{
			const SweepInsn &s = v[i];
			if (!stop && s.m >= ZYDIS_MNEMONIC_JB && s.m <= ZYDIS_MNEMONIC_JZ &&
			    s.tgt)
			{
				stop = s.tgt;
				continue;
			}
			if (stop && s.p >= stop)
				return;
			if (s.m == ZYDIS_MNEMONIC_CALL && s.tgt)
				out.push_back({i, s.tgt});
		}
	}

	static bool key_is_by_value(const std::vector<SweepInsn> &v, int ctor_idx,
	                            int op_idx)
	{
		ZydisRegister base = ZYDIS_REGISTER_NONE;
		int64_t disp = 0;
		bool found = false;
		for (int i = ctor_idx - 1; i >= 0 && i >= ctor_idx - 12; --i)
			if (v[i].m == ZYDIS_MNEMONIC_LEA && v[i].has_mem)
			{
				base = v[i].mem_base;
				disp = v[i].mem_disp;
				found = true;
				break;
			}
		if (!found)
			return false;

		for (int i = ctor_idx + 1; i < op_idx; ++i)
		{
			if (!v[i].has_mem || v[i].mem_base != base || v[i].mem_disp != disp)
				continue;
			if (v[i].m == ZYDIS_MNEMONIC_MOV)
				return true;
			if (v[i].m == ZYDIS_MNEMONIC_LEA)
				return false;
		}
		return false;
	}

	static bool resolve_section_ops(const anchor::ModuleImage &img,
	                                void **out_add, void **out_remove_key,
	                                const uint8_t **out_combine,
	                                void **out_key_ctor, bool *out_fname_keyed)
	{
		static const uint64_t kDirs[4] = {0x2b, 0x2d, 0x2e, 0x21};

		for (void *fn : anchor::functions_referencing_wstr(img, L"%GAME%"))
		{
			const uint8_t *b = (const uint8_t *)fn;
			const uint8_t *e = anchor::function_end(img, fn);
			std::vector<SweepInsn> v;
			if (!e || !sweep(b, e, v))
				continue;

			std::vector<std::pair<int, const uint8_t *>> calls[4];
			bool all = true;
			for (int d = 0; d < 4; ++d)
			{
				int ci = last_cmp_idx(v, kDirs[d]);
				if (ci < 0)
				{
					all = false;
					break;
				}
				branch_calls(v, ci, calls[d]);
				if (calls[d].empty())
					all = false;
			}
			if (!all)
				continue;

			const uint8_t *ctor = nullptr;
			for (auto &c : calls[0])
			{
				bool in_all = true;
				for (int d = 1; d < 4 && in_all; ++d)
				{
					in_all = false;
					for (auto &o : calls[d])
						if (o.second == c.second)
							in_all = true;
				}
				if (in_all)
					ctor = c.second;
			}
			if (!ctor)
				continue;

			auto op_slot = [&](int d, int *ctor_i, int *op_i) -> void *
			{
				int last = -1;
				for (int i = 0; i < (int)calls[d].size(); ++i)
					if (calls[d][i].second == ctor)
						last = i;
				if (last < 0 || last + 1 >= (int)calls[d].size())
					return nullptr;
				if (ctor_i)
					*ctor_i = calls[d][last].first;
				if (op_i)
					*op_i = calls[d][last + 1].first;
				return (void *)calls[d][last + 1].second;
			};

			int rem_ctor_i = -1, rem_op_i = -1;
			void *add = op_slot(2, nullptr, nullptr);        // '.'
			void *rem = op_slot(3, &rem_ctor_i, &rem_op_i);  // '!'
			if (!add || !rem || add == rem)
				continue;

			*out_add = add;
			*out_remove_key = rem;
			*out_combine = b;
			*out_key_ctor = (void *)ctor;
			*out_fname_keyed = key_is_by_value(v, rem_ctor_i, rem_op_i);
			log_info("lua: localize - keyctor=%p fname_keyed=%d",
			         (const void *)ctor, (int)*out_fname_keyed);
			return true;
		}
		return false;
	}

	using GetSectionFn = void *(__fastcall *)(void *self,
	                                          const wchar_t *Section, int Force,
	                                          int Const,
	                                          const wchar_t *Filename);

	void *__fastcall get_section_detour(void *self, const wchar_t *Section,
	                                    int Force, int Const,
	                                    const wchar_t *Filename)
	{
		std::wstring pkg = package_from_filename(Filename).c_str();
		void *Sec = reinterpret_cast<GetSectionFn>(g_orig_get_section)(
		    self, Section, Force, Const, Filename);
		if (Sec && Section && Filename && !pkg.empty())
		{
			std::wstring prefix = loc_prefix(pkg.c_str(), Section);

			std::lock_guard<std::mutex> lk(g_loc_mtx);

			for (const auto &[k, v] : g_loc_overrides)
			{
				if (k.compare(0, prefix.size(), prefix) == 0)
				{
					if (g_remove_key && g_add && (!g_fname_keyed || g_key_ctor))
					{
						size_t key_start = prefix.size();
						size_t key_end = k.find(L'\x01', key_start);

						if (key_end == std::wstring::npos)
							continue;

						std::wstring key =
						    k.substr(key_start, key_end - key_start);

						EngFString ValStr{};
						build_eng_fstring(&ValStr, std::wstring(v.c_str()));

						if (g_fname_keyed)
						{
							FNameStack fn{};
							reinterpret_cast<FNameInitFn>(g_key_ctor)(
							    &fn, key.c_str(), 0, 1, 1);

							reinterpret_cast<RemoveKeyFNFn>(g_remove_key)(Sec,
							                                              fn);

							int idx = 0;
							PairInitFN kv{fn, &ValStr};
							reinterpret_cast<AddFNFn>(g_add)(Sec, &idx, &kv,
							                                 nullptr);
						}
						else
						{
							EngFString KeyStr{};
							build_eng_fstring(&KeyStr, key);
							reinterpret_cast<RemoveKeyFn>(g_remove_key)(
							    Sec, &KeyStr);
							reinterpret_cast<AddFn>(g_add)(Sec, &KeyStr,
							                               &ValStr);
						}
					}
					else
					{
						log_warn(
						    "lua: localize - Section functions not resolved, "
						    "skipping patch");
					}
				}
			}
		}

		return Sec;
	}

	using LocalizeFn = void *(__cdecl *)(void *, const wchar_t *,
	                                     const wchar_t *, const wchar_t *,
	                                     const wchar_t *, int);

	void *__cdecl localize_detour(void *ret, const wchar_t *Section,
	                              const wchar_t *Key, const wchar_t *Package,
	                              const wchar_t *Lang, int bOpt)
	{
		if (Section && Key && Package)
		{
			std::wstring v;
			bool have = false;
			{
				std::lock_guard<std::mutex> lk(g_loc_mtx);
				auto it = g_loc_overrides.find(loc_key(Package, Section, Key));
				if (it != g_loc_overrides.end())
				{
					v = it->second;
					have = true;
				}
			}
			if (have)
			{
				build_eng_fstring((EngFString *)ret, v);
				return ret;
			}
		}
		return reinterpret_cast<LocalizeFn>(g_orig_localize)(
		    ret, Section, Key, Package, Lang, bOpt);
	}

	bool ensure_localize_hook()
	{
		static bool tried = false, ok = false;
		if (tried)
			return ok;
		tried = true;
		static const wchar_t *loc_kAnchors[] = {L"?%s?%s.%s.%s?",
		                                        L"<?%s?%s.%s.%s?>"};
		static void *loc_addr =
		    ue3_api::resolve_wstr_any(loc_kAnchors, 2, "Localize");
		if (!loc_addr)
		{
			log_warn("lua: localize - Localize not resolved");
			return false;
		}
		if (!ue3().ArrayRealloc)
		{
			log_warn("lua: localize - ArrayRealloc unresolved");
			return false;
		}

		static const wchar_t *sec_kAnchors[] = {L"UnrealEd.EditorEngine",
		                                        L"Editor.EditorEngine"};
		static void *loadallclasses_addr =
		    ue3_api::resolve_wstr_any(sec_kAnchors, 2, "LoadAllClasses");

		void *get_section_addr = nullptr;
		if (loadallclasses_addr)
		{
			anchor::ModuleImage img = anchor::image_of(nullptr);
			if (img.ok)
			{

				const uint8_t *combine = nullptr;
				if (resolve_section_ops(img, &g_add, &g_remove_key, &combine,
				                        &g_key_ctor, &g_fname_keyed))
				{
					log_info("lua: localize - Combine=%p Add=%p RemoveKey=%p",
					         combine, g_add, g_remove_key);
				}
				else
				{
					g_add = g_remove_key = g_key_ctor = nullptr;
					g_fname_keyed = false;
					log_warn("lua: localize - Combine/section ops unresolved");
				}

				get_section_addr =
				    anchor::nth_call_target(img, loadallclasses_addr, 0);
			}
			if (get_section_addr)
				log_info("lua: localize - GetSectionPrivate resolved = %p",
				         get_section_addr);
			else
				log_warn("lua: localize - GetSectionPrivate not found via "
				         "nth_call_target");
		}

		hook::add(loc_addr, (void *)&localize_detour, &g_orig_localize);
		hook::add(get_section_addr, (void *)&get_section_detour,
		          &g_orig_get_section);
		hook::install_all();
		ok = (g_orig_localize != nullptr);
		log_info(ok ? "lua: Localize hook installed (localize2)"
		            : "lua: localize2 - hook install failed");
		return ok;
	}

	void bind_ue3(sol::state &lua)
	{
		lua.set_function("print",
		                 [](sol::this_state s, sol::variadic_args va)
		                 {
			                 sol::state_view sv(s);
			                 std::string line;
			                 for (auto v : va)
			                 {
				                 std::string piece = sv["tostring"](v);
				                 if (!line.empty())
					                 line += '\t';
				                 line += piece;
			                 }
			                 log_info("[lua] %s", line.c_str());
		                 });

		sol::table tbl = lua["ue3"].get_or_create<sol::table>();

		tbl.set_function("log", [](const std::string &m)
		                 { log_info("[lua] %s", m.c_str()); });
		tbl.set_function("warn", [](const std::string &m)
		                 { log_warn("[lua] %s", m.c_str()); });
		tbl.set_function("error", [](const std::string &m)
		                 { log_err("[lua] %s", m.c_str()); });

		tbl.set_function("build",
		                 [&lua]()
		                 {
			                 return lua.create_table_with(
			                     "arch", sizeof(void *) == 8 ? "x64" : "x86",
			                     "loader", "CU3ML");
		                 });

		tbl.set_function("cmdline", []() { return to_narrow(g_cmdline); });
		tbl.set_function("has_switch",
		                 [](const std::string &name)
		                 {
			                 std::wstring w = lower(to_wide(name));
			                 for (const auto &sw : g_switches)
				                 if (sw == w)
					                 return true;
			                 return false;
		                 });

		tbl.set_function("mods",
		                 [&lua]()
		                 {
			                 sol::table t = lua.create_table();
			                 int i = 1;
			                 for (const auto &lm : mod_loader::loaded_mods())
				                 t[i++] = lua.create_table_with(
				                     "name", lm.cfg.name, "version",
				                     lm.cfg.version, "author", lm.cfg.author);
			                 return t;
		                 });

		tbl.set_function(
		    "msgbox",
		    [](const std::string &text, sol::optional<std::string> caption)
		    {
			    std::wstring t = to_wide(text);
			    std::wstring c = to_wide(caption.value_or("CU3ML"));
			    MessageBoxW(nullptr, t.c_str(), c.c_str(), MB_OK);
		    });

		tbl.set_function(
		    "layout",
		    [&lua]()
		    {
			    const UE3Layout &L = ue3();
			    auto hex = [](const void *p)
			    {
				    char b[32];
				    snprintf(b, sizeof(b), "%p", p);
				    return std::string(b);
			    };
			    return lua.create_table_with(
			        "ok", L.ok, "StaticLoadObject", hex(L.StaticLoadObject),
			        "StaticFindObjectFast", hex(L.StaticFindObjectFast),
			        "GetPackageLinker", hex(L.GetPackageLinker), "Preload",
			        hex(L.Preload), "FNameInit", hex(L.FNameInit),
			        "GPackageFileCache",
			        hex(static_cast<void *>(L.GPackageFileCache)));
		    });

		tbl.set_function(
		    "load",
		    [](const std::string &path,
		       sol::optional<void *> outer) -> sol::optional<void *>
		    {
			    void *slo = ue3().StaticLoadObject;
			    if (!slo)
			    {
				    log_warn("lua: ue3.load — StaticLoadObject unresolved");
				    return sol::nullopt;
			    }
			    using Fn = void *(__cdecl *)(void *, void *, const wchar_t *,
			                                 const wchar_t *, uint32_t, void *,
			                                 int32_t);
			    std::wstring w = to_wide(path);
			    void *res = reinterpret_cast<Fn>(slo)(
			        /*InClass*/ nullptr, /*InOuter*/ outer.value_or(nullptr),
			        w.c_str(), /*Filename*/ nullptr, /*LoadFlags*/ 0,
			        /*Sandbox*/ nullptr, /*bAllowReconciliation*/ 1);
			    if (!res)
				    return sol::nullopt;
			    return res;
		    });

		tbl.set_function(
		    "name",
		    [&lua](const std::string &s) -> sol::object
		    {
			    void *init = ue3().FNameInit;
			    if (!init)
			    {
				    log_warn("lua: ue3.name — FName::Init unresolved");
				    return sol::lua_nil;
			    }
			    struct FN
			    {
				    int32_t Index, Number;
			    } out{0, 0};
			    std::wstring w = to_wide(s);
			    using Fn = void(UE3_THISCALL *)(void *, const wchar_t *, int,
			                                    int, int);
			    reinterpret_cast<Fn>(init)(&out, w.c_str(), 0, /*FNAME_Add*/ 1,
			                               /*bSplitName*/ 1);
			    return lua.create_table_with("index", out.Index, "number",
			                                 out.Number);
		    });

		tbl.set_function("fname",
		                 [](int index) -> sol::optional<std::string>
		                 {
			                 UE3Layout &L = ue3();
			                 void *e = fname_entry(index);
			                 if (!e || !L.name.str_off)
				                 return sol::nullopt;
			                 if (L.name.is_unicode(e))
				                 return to_narrow(L.name.uni(e));
			                 return std::string(L.name.ansi(e));
		                 });

		tbl.set_function("localize",
		                 [](const std::string &section, const std::string &key,
		                    const std::string &package,
		                    const std::string &new_string) -> bool
		                 {
			                 if (!ensure_localize_hook())
				                 return false;
			                 std::wstring wk = loc_key(to_wide(package).c_str(),
			                                           to_wide(section).c_str(),
			                                           to_wide(key).c_str());
			                 std::lock_guard<std::mutex> lk(g_loc_mtx);
			                 g_loc_overrides[wk] = to_wide(new_string);
			                 return true;
		                 });

		tbl.set_function(
		    "set_engine_free",
		    [](sol::object v)
		    {
			    if (v.is<void *>())
			    {
				    ue3_api::set_engine_free(v.as<void *>());
				    return;
			    }
			    if (v.is<uintptr_t>())
			    {
				    auto *base =
				        reinterpret_cast<uint8_t *>(GetModuleHandleW(nullptr));
				    ue3_api::set_engine_free(base + v.as<uintptr_t>());
				    return;
			    }
			    log_warn("lua: set_engine_free wants an RVA int or pointer");
		    });

		tbl.set_function(
		    "module_base", []() -> uintptr_t
		    { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); });

		tbl.set_function(
		    "exec",
		    [](const std::string &cmd)
		        -> std::tuple<sol::optional<std::string>, bool>
		    {
			    static ue3_api::EngineFn<int(const wchar_t *, void *)> exec_fn{
			        L"SETTRACKINGBASELINE", "StaticExec"};
			    if (!exec_fn)
				    return {sol::nullopt, false};

			    std::wstring w = to_wide(cmd);
			    ue3_api::CaptureOutputDevice ar;
			    int handled = exec_fn(w.c_str(), ar.device());
			    return {ar.take(), handled != 0};
		    });

		{
			sol::table input = tbl["input"].get_or_create<sol::table>();

			input.set_function(
			    "exec",
			    [](void *self, const std::string &cmd)
			        -> std::tuple<sol::optional<std::string>, bool>
			    {
				    static ue3_api::EngineFn<int UE3_THISCALL(
				        void *, const wchar_t *, void *)>
				        fn{L"Bad Button command", "UInput::Exec"};
				    if (!self || !fn)
					    return {sol::nullopt, false};
				    std::wstring w = to_wide(cmd);
				    ue3_api::CaptureOutputDevice ar;
				    int handled = fn(self, w.c_str(), ar.device());
				    return {ar.take(), handled != 0};
			    });

			input.set_function(
			    "exec_commands",
			    [](void *self,
			       const std::string &cmd) -> sol::optional<std::string>
			    {
				    static ue3_api::EngineFn<void UE3_THISCALL(
				        void *, const wchar_t *, void *)>
				        fn{L"OnRelease", "UInput::ExecInputCommands"};
				    if (!self || !fn)
					    return sol::nullopt;
				    std::wstring w = to_wide(cmd);
				    ue3_api::CaptureOutputDevice ar;
				    fn(self, w.c_str(), ar.device());
				    return ar.take();
			    });
		}

		{
			sol::table config = tbl["config"].get_or_create<sol::table>();

			config.set_function(
			    "load_file",
			    [](void *gconfig, const std::string &filename)
			    {
				    static ue3_api::EngineFn<void UE3_THISCALL(
				        void *, const wchar_t *, const void *)>
				        fn{L"FConfigCacheIni::LoadFile failed loading file as "
				           L"it was 0 size.  Filename was:  %s",
				           "FConfigCacheIni::LoadFile"};
				    if (!gconfig || !fn)
					    return;
				    std::wstring w = to_wide(filename);
				    fn(gconfig, w.c_str(), /*Fallback*/ nullptr);
			    });
		}

		log_info("lua: `ue3` table bound");
	}

	void run_startup_scripts()
	{
		for (const auto &lm : mod_loader::loaded_mods())
		{
			std::wstring path = lm.dir_w + L"\\main.lua";
			if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
			{
				log_info("lua: running mod script %ls", path.c_str());
				lua_host::run_file(path);
			}
		}

		for (const auto &f : g_lua_files)
		{
			log_info("lua: running -lua script %ls", f.c_str());
			lua_host::run_file(f);
		}
	}
}  // namespace

namespace lua_host
{
	void configure_from_cmdline()
	{
		if (g_cfg_done)
			return;
		g_cfg_done = true;

		const wchar_t *raw = GetCommandLineW();
		g_cmdline = raw ? raw : L"";

		for (const auto &tok : tokenize(g_cmdline))
		{
			if (tok.empty() || (tok[0] != L'-' && tok[0] != L'/'))
				continue;
			std::wstring body = lower(tok.substr(1));

			if (body == L"nolua")
				g_enabled = false;
			else if (body == L"luaverbose")
				g_verbose = true;
			else if (body.rfind(L"lua=", 0) == 0)
			{
				std::wstring path = tok.substr(1 + 4);
				if (!path.empty())
					g_lua_files.push_back(path);
				g_switches.push_back(L"lua");
			}
			else
				g_switches.push_back(body);
		}

		log_info(
		    "lua: cmdline parsed — enabled=%d verbose=%d extra_scripts=%zu",
		    (int)g_enabled, (int)g_verbose, g_lua_files.size());
	}

	void init()
	{
		configure_from_cmdline();

		if (!g_enabled)
		{
			log_info("lua: disabled via -nolua");
			return;
		}
		if (g_lua)
			return;

		try
		{
			g_lua = new sol::state();
			g_lua->open_libraries(sol::lib::base, sol::lib::package,
			                      sol::lib::string, sol::lib::table,
			                      sol::lib::math, sol::lib::os, sol::lib::io,
			                      sol::lib::coroutine);
			bind_ue3(*g_lua);
			log_info("lua: VM ready (sol %d.%d.%d, Lua %s.%s.%s)",
			         SOL_VERSION_MAJOR, SOL_VERSION_MINOR, SOL_VERSION_PATCH,
			         LUA_VERSION_MAJOR, LUA_VERSION_MINOR, LUA_VERSION_RELEASE);

			run_startup_scripts();
		}
		catch (const std::exception &e)
		{
			log_err("lua: init failed: %s", e.what());
			shutdown();
		}
		catch (...)
		{
			log_err("lua: init failed (unknown exception)");
			shutdown();
		}
	}

	void shutdown()
	{
		if (g_lua)
		{
			delete g_lua;
			g_lua = nullptr;
			log_info("lua: VM shut down");
		}
	}

	bool enabled() { return g_enabled; }

	bool initialized() { return g_lua != nullptr; }

	bool run_string(const std::string &code, const char *chunk_name)
	{
		if (!g_lua)
		{
			log_warn("lua: run_string before init");
			return false;
		}
		bool ok = run_chunk(code, chunk_name ? chunk_name : "=chunk");
		if (g_verbose)
			log_info("lua: run_string(%s) -> %d", chunk_name, (int)ok);
		return ok;
	}

	bool run_file(const std::wstring &path)
	{
		if (!g_lua)
		{
			log_warn("lua: run_file before init");
			return false;
		}

		HANDLE h =
		    CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
		                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
		{
			log_err("lua: cannot open %ls (err=%lu)", path.c_str(),
			        GetLastError());
			return false;
		}

		std::string data;
		LARGE_INTEGER sz{};
		if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 &&
		    sz.QuadPart < (64 << 20))
		{
			data.resize(static_cast<size_t>(sz.QuadPart));
			DWORD got = 0;
			ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &got,
			         nullptr);
			data.resize(got);
		}
		CloseHandle(h);

		std::string name = "@" + to_narrow(path);
		bool ok = run_chunk(data, name.c_str());
		if (g_verbose)
			log_info("lua: run_file(%ls) -> %d", path.c_str(), (int)ok);
		return ok;
	}

}  // namespace lua_host
