#define SOL_ALL_SAFETIES_ON 1
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <sol/sol.hpp>

#include "addr_cache.hpp"
#include "loader_config.hpp"
#include "util.hpp"

#include <atomic>
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

#include "lua_lib.hpp"

#include "manager/msg_box.hpp"

namespace
{
	std::vector<sol::protected_function> g_map_cbs;
	void *g_orig_loadmap = nullptr;

	void fire_map_callbacks()
	{
		for (auto &fn : g_map_cbs)
		{
			try
			{
				auto r = fn();
				if (!r.valid())
				{
					sol::error e = r;
					log_err("lua: on_map_load callback: %s", e.what());
				}
			}
			catch (const std::exception &e)
			{
				log_err("lua: on_map_load callback threw: %s", e.what());
			}
			catch (...)
			{
				log_err("lua: on_map_load callback threw");
			}
		}
	}

#ifdef _WIN64
	using LoadMapFn = int (*)(void *, const void *, void *, void *);

	int loadmap_detour(void *self, const void *URL, void *Pending, void *Error)
	{
		const int r = reinterpret_cast<LoadMapFn>(g_orig_loadmap)(
		    self, URL, Pending, Error);
#else
	using LoadMapFn = int(__fastcall *)(void *, void *, const void *, void *,
	                                    void *);

	int __fastcall loadmap_detour(void *self, void *edx, const void *URL,
	                              void *Pending, void *Error)
	{
		const int r = reinterpret_cast<LoadMapFn>(g_orig_loadmap)(
		    self, edx, URL, Pending, Error);
#endif
		log_info("lua: LoadMap returned %d, firing %zu callback(s)", r,
		         g_map_cbs.size());
		fire_map_callbacks();
		return r;
	}

	bool ensure_map_hook()
	{
		static bool tried = false;
		if (tried)
			return g_orig_loadmap != nullptr;
		tried = true;

		static const wchar_t *kAnchors[] = {L"Game=", L"quiet", L"TheWorld"};
		void *fn =
		    ue3_api::resolve_wstr_all(kAnchors, 3, "UGameEngine::LoadMap");
		if (!fn)
		{
			log_warn("lua: on_map_load — LoadMap not resolved");
			return false;
		}
		hook::add(fn, (void *)&loadmap_detour, &g_orig_loadmap);
		hook::install_all();
		return g_orig_loadmap != nullptr;
	}
}  // namespace

namespace
{
	struct PreloadWatch
	{
		std::string path;
		sol::protected_function fn;
		sol::object payload;
		bool once = true;
	};

	inline uint64_t name_key(int32_t idx, int32_t num)
	{
		return ((uint64_t)(uint32_t)idx << 32) | (uint32_t)num;
	}

	std::unordered_map<uint64_t, std::vector<PreloadWatch>> g_preload_watch;
	std::atomic<size_t> g_preload_live{0};
	std::mutex g_preload_mtx;

	sol::state *g_lua = nullptr;
	bool g_enabled = true;
	bool g_verbose = false;
	bool g_cfg_done = false;
	bool g_nolua_switch = false;

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

	std::string lower_ascii(std::string s)
	{
		for (auto &c : s)
			c = towlower(c);
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

	void *g_gconfig = nullptr;
	std::unordered_map<std::wstring, std::wstring> g_file_by_pkg;

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

	using SetStringFn = void(UE3_THISCALL *)(void *self, const wchar_t *Section,
	                                         const wchar_t *Key,
	                                         const wchar_t *Value,
	                                         const wchar_t *Filename);

	using SetArrayFn = void(UE3_THISCALL *)(void *self, const wchar_t *Section,
	                                        const wchar_t *Key,
	                                        const UE3TArray *Value,
	                                        const wchar_t *Filename);

	using GetArrayFn = int(UE3_THISCALL *)(void *self, const wchar_t *Section,
	                                       const wchar_t *Key,
	                                       UE3TArray *Result,
	                                       const wchar_t *Filename);

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

	struct SecOp
	{
		std::wstring pkg;
		std::wstring file;
		std::wstring section;
		std::wstring key;
		std::wstring value;
		bool add = true;
		bool remove = true;
	};

	std::vector<SecOp> g_sec_pending;
	std::vector<SecOp> g_sec_applied;

	bool sec_ops_ready()
	{
		return g_remove_key && g_add && (!g_fname_keyed || g_key_ctor);
	}

	void *cfg_self()
	{
		if (!g_gconfig && ue3().GConfig)
			g_gconfig = *ue3().GConfig;
		return g_gconfig;
	}

	const std::wstring *cfg_file_for(const SecOp &op)
	{
		if (!op.file.empty())
			return &op.file;
		auto it = g_file_by_pkg.find(lower(op.pkg));
		return it == g_file_by_pkg.end() ? nullptr : &it->second;
	}

	void *cfg_section(const wchar_t *Section, const wchar_t *Filename,
	                  bool create)
	{
		void *self = cfg_self();
		if (!self || !g_orig_get_section || !Section || !Filename)
			return nullptr;
		auto fn = reinterpret_cast<GetSectionFn>(g_orig_get_section);
		void *Sec = fn(self, Section, 0, 1, Filename);
		if (!Sec && create)
			Sec = fn(self, Section, 1, 0, Filename);
		return Sec;
	}

	bool ieq(const std::wstring &a, const wchar_t *b)
	{
		return b && lower(a) == lower(std::wstring(b));
	}

	bool matches_file(const SecOp &op, const wchar_t *Filename)
	{
		if (!Filename)
			return false;
		if (!op.file.empty())
			return ieq(op.file, Filename);
		if (op.pkg.empty())
			return false;
		return lower(op.pkg) == lower(package_from_filename(Filename));
	}

	void sec_apply_to(void *Sec, const SecOp &op)
	{
		if (!Sec || !sec_ops_ready())
			return;

		EngFString ValStr{};
		if (op.add)
			build_eng_fstring(&ValStr, op.value);

		if (g_fname_keyed)
		{
			FNameStack fn{};
			reinterpret_cast<FNameInitFn>(g_key_ctor)(&fn, op.key.c_str(), 0, 1,
			                                          1);
			if (op.remove)
				reinterpret_cast<RemoveKeyFNFn>(g_remove_key)(Sec, fn);
			if (op.add)
			{
				int idx = 0;
				PairInitFN kv{fn, &ValStr};
				reinterpret_cast<AddFNFn>(g_add)(Sec, &idx, &kv, nullptr);
			}
		}
		else
		{
			EngFString KeyStr{};
			build_eng_fstring(&KeyStr, op.key);
			if (op.remove)
				reinterpret_cast<RemoveKeyFn>(g_remove_key)(Sec, &KeyStr);
			if (op.add)
				reinterpret_cast<AddFn>(g_add)(Sec, &KeyStr, &ValStr);
		}
	}

	bool sec_apply(const SecOp &op)
	{
		if (!sec_ops_ready())
			return false;
		const std::wstring *file = cfg_file_for(op);
		if (!file)
			return false;

		void *Sec = cfg_section(op.section.c_str(), file->c_str(), op.add);
		if (!Sec)
			return false;

		sec_apply_to(Sec, op);
		log_info("lua: section - %ls[%ls].%ls add=%d remove=%d applied",
		         file->c_str(), op.section.c_str(), op.key.c_str(), (int)op.add,
		         (int)op.remove);
		return true;
	}

	void sec_flush_pending()
	{
		if (g_sec_pending.empty())
			return;
		std::vector<SecOp> keep;
		for (auto &op : g_sec_pending)
		{
			if (sec_apply(op))
				g_sec_applied.push_back(op);
			else
				keep.push_back(std::move(op));
		}
		g_sec_pending.swap(keep);
	}

	void *__fastcall get_section_detour(void *self, const wchar_t *Section,
	                                    int Force, int Const,
	                                    const wchar_t *Filename)
	{
		if (!g_gconfig && self)
			g_gconfig = self;

		void *Sec = reinterpret_cast<GetSectionFn>(g_orig_get_section)(
		    self, Section, Force, Const, Filename);

		std::wstring pkg = package_from_filename(Filename);
		std::lock_guard<std::mutex> lk(g_loc_mtx);

		if (!pkg.empty() && Filename)
			g_file_by_pkg[lower(pkg)] = Filename;

		if (Sec && Section && Filename && !pkg.empty() && sec_ops_ready())
		{
			const std::wstring prefix = loc_prefix(pkg.c_str(), Section);

			for (const auto &[k, v] : g_loc_overrides)
			{
				if (k.size() < prefix.size() ||
				    k.compare(0, prefix.size(), prefix) != 0)
					continue;

				const size_t key_end = k.find(L'\x01', prefix.size());
				if (key_end == std::wstring::npos)
					continue;

				SecOp op;
				op.key = k.substr(prefix.size(), key_end - prefix.size());
				op.value = v;
				op.add = true;
				op.remove = true;
				sec_apply_to(Sec, op);
			}

			for (const SecOp &op : g_sec_applied)
				if (op.add && op.remove && ieq(op.section, Section) &&
				    matches_file(op, Filename))
					sec_apply_to(Sec, op);
		}
		else if (Sec && !sec_ops_ready())
		{
			log_warn("lua: localize - Section functions not resolved, "
			         "skipping patch");
		}

		sec_flush_pending();

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

	using FindFileFn = void *(UE3_THISCALL *)(void *, const wchar_t *, int);
	static void *g_orig_find_file = nullptr;

	void *__fastcall find_file_detour(void *self,
	                                  UE3_EDX const wchar_t *Filename,
	                                  int CreateIfNotFound)
	{
		void *File = reinterpret_cast<FindFileFn>(g_orig_find_file)(
		    self, Filename, CreateIfNotFound);

		if (!g_gconfig && self)
			g_gconfig = self;
		static thread_local bool in_flush = false;
		if (in_flush || !File || !Filename)
			return File;

		std::wstring pkg = package_from_filename(Filename);
		if (pkg.empty())
			return File;

		in_flush = true;
		{
			std::lock_guard<std::mutex> lk(g_loc_mtx);
			g_file_by_pkg[lower(pkg)] = Filename;
			sec_flush_pending();
		}
		in_flush = false;
		return File;
	}

	static void resolve_lua_addrs(LuaAddrLayout &L)
	{
		static const wchar_t *loc_kAnchors[] = {L"?%s?%s.%s.%s?",
		                                        L"<?%s?%s.%s.%s?>"};
		L.Localize = ue3_api::resolve_wstr_any(loc_kAnchors, 2, "Localize");
		if (!L.Localize)
		{
			log_warn("lua: Localize not resolved");
			return;
		}

		static const wchar_t *sec_kAnchors[] = {L"UnrealEd.EditorEngine",
		                                        L"Editor.EditorEngine"};
		L.LoadAllClasses =
		    ue3_api::resolve_wstr_any(sec_kAnchors, 2, "LoadAllClasses");
		if (!L.LoadAllClasses)
		{
			log_warn("lua: LoadAllClasses anchor not found");
			return;
		}

		anchor::ModuleImage img = anchor::image_of(nullptr);
		if (!img.ok)
		{
			log_warn("lua: module image unavailable");
			return;
		}

		const uint8_t *combine = nullptr;
		if (resolve_section_ops(img, &L.SectionAdd, &L.SectionRemoveKey,
		                        &combine, &L.KeyCtor, &L.fname_keyed))
		{
			L.Combine = (void *)combine;
			log_info("lua: Combine=%p Add=%p RemoveKey=%p", L.Combine,
			         L.SectionAdd, L.SectionRemoveKey);
		}
		else
		{
			L.SectionAdd = L.SectionRemoveKey = L.KeyCtor = nullptr;
			L.Combine = nullptr;
			L.fname_keyed = false;
			log_warn("lua: Combine/section ops unresolved");
		}

		L.GetSectionPrivate = anchor::nth_call_target(img, L.LoadAllClasses, 0);
		if (!L.GetSectionPrivate)
		{
			log_warn("lua: GetSectionPrivate not found via "
			         "nth_call_target");
			return;
		}
		log_info("lua: GetSectionPrivate resolved = %p", L.GetSectionPrivate);

		void *find = anchor::nth_call_target(img, L.GetSectionPrivate, 0);
		if (!find || find == L.GetSectionPrivate)
		{
			log_warn("lua: FConfigCacheIni::Find not found "
			         "(LoadAllClasses=%p GetSectionPrivate=%p)",
			         L.LoadAllClasses, L.GetSectionPrivate);
			return;
		}
		L.FindFile = find;
		log_info("lua: Find=%p GetSectionPrivate=%p", L.FindFile,
		         L.GetSectionPrivate);
	}

	bool ensure_lua_hooks()
	{
		static bool tried = false, ok = false;
		if (tried)
			return ok;
		tried = true;

		if (!ue3().ArrayRealloc)
		{
			log_warn("lua: ArrayRealloc unresolved");
			return false;
		}

		LuaAddrLayout L;
		const bool from_cache = addr_cache::load_lua(L);
		if (!from_cache)
			resolve_lua_addrs(L);

		if (!L.Localize)
			return false;

		g_add = L.SectionAdd;
		g_remove_key = L.SectionRemoveKey;
		g_key_ctor = L.KeyCtor;
		g_fname_keyed = L.fname_keyed;

		if (L.FindFile)
			hook::add(L.FindFile, (void *)&find_file_detour, &g_orig_find_file);
		if (L.GetSectionPrivate)
			hook::add(L.GetSectionPrivate, (void *)&get_section_detour,
			          &g_orig_get_section);
		hook::add(L.Localize, (void *)&localize_detour, &g_orig_localize);
		hook::install_all();

		ok = (g_orig_localize != nullptr);
		if (!ok)
		{
			log_err("lua: hook install failed");
			if (from_cache)
				addr_cache::invalidate("lua hooks failed to install");
			return false;
		}

		log_info("lua: lua hook installed%s", from_cache ? " [cached]" : "");

		L.ok = L.Localize && L.GetSectionPrivate && L.FindFile &&
		       L.SectionAdd && L.SectionRemoveKey &&
		       (!L.fname_keyed || L.KeyCtor);
		if (!from_cache && L.ok)
		{
			addr_cache::store_lua(L);
			addr_cache::save();
		}

		return ok;
	}

	bool sec_submit(SecOp op)
	{
		if (!ensure_lua_hooks())
			return false;
		std::lock_guard<std::mutex> lk(g_loc_mtx);
		if (sec_apply(op))
			return true;
		g_sec_pending.push_back(std::move(op));
		return true;
	}

	bool safe_read(const void *p, size_t n) { return p && !IsBadReadPtr(p, n); }

	std::string fname_str(int32_t gidx)
	{
		UE3Layout &L = ue3();
		void *e = fname_entry(gidx);
		if (!e || !L.name.str_off || !safe_read(e, L.name.str_off + 2))
			return {};
		if (L.name.is_unicode(e))
		{
			const wchar_t *s = L.name.uni(e);
			return safe_read(s, 2) ? to_narrow(std::wstring(s)) : std::string{};
		}
		const char *s = L.name.ansi(e);
		return safe_read(s, 1) ? std::string(s) : std::string{};
	}

	std::string name_of(const void *obj)
	{
		if (!obj || !safe_read(obj, (size_t)ue3().sizeof_UObject))
			return {};
		std::string s = fname_str(uobj_name_index(obj));
		const int32_t num = uobj_name_number(obj);
		if (num > 0)
			s += "_" + std::to_string(num - 1);
		return s;
	}

	std::string path_of(const void *obj)
	{
		std::vector<std::string> parts;
		const void *cur = obj;
		for (int d = 0; d < 16 && cur; ++d)
		{
			std::string seg = name_of(cur);
			if (seg.empty())
				return {};
			parts.push_back(seg);
			cur = uobj_outer(cur);
		}
		std::string out;
		for (auto it = parts.rbegin(); it != parts.rend(); ++it)
		{
			if (!out.empty())
				out += '.';
			out += *it;
		}
		return out;
	}

	bool path_matches(const std::string &full, const std::string &want)
	{
		if (want.empty() || full == want)
			return true;
		return full.size() > want.size() &&
		       full.compare(full.size() - want.size(), want.size(), want) ==
		           0 &&
		       full[full.size() - want.size() - 1] == '.';
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
			                 for (const auto &lm : mod_loader::enabled_mods())
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
			        nullptr, outer.value_or(nullptr), w.c_str(), nullptr, 0,
			        nullptr, 1);
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
			    reinterpret_cast<Fn>(init)(&out, w.c_str(), 0, 1, 1);
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
			sol::table sec_tbl = tbl["section"].get_or_create<sol::table>();

			auto make_op =
			    [](const std::string &section, const std::string &key,
			       const std::string &package, const std::string &value,
			       sol::optional<std::string> file, bool add, bool remove)
			{
				SecOp op;
				op.pkg = to_wide(package);
				op.file = file ? to_wide(*file) : std::wstring{};
				op.section = to_wide(section);
				op.key = to_wide(key);
				op.value = to_wide(value);
				op.add = add;
				op.remove = remove;
				return op;
			};

			sec_tbl.set_function(
			    "localize",
			    [](const std::string &section, const std::string &key,
			       const std::string &package,
			       const std::string &new_string) -> bool
			    {
				    if (!ensure_lua_hooks())
					    return false;
				    std::wstring wk =
				        loc_key(to_wide(package).c_str(),
				                to_wide(section).c_str(), to_wide(key).c_str());
				    std::lock_guard<std::mutex> lk(g_loc_mtx);
				    g_loc_overrides[wk] = to_wide(new_string);
				    return true;
			    });

			sec_tbl.set_function(
			    "set",
			    [make_op](const std::string &section, const std::string &key,
			              const std::string &package,
			              const std::string &new_string,
			              sol::optional<std::string> file)
			    {
				    return sec_submit(make_op(section, key, package, new_string,
				                              file, true, true));
			    });

			sec_tbl.set_function(
			    "add",
			    [make_op](const std::string &section, const std::string &key,
			              const std::string &package,
			              const std::string &new_string,
			              sol::optional<std::string> file)
			    {
				    return sec_submit(make_op(section, key, package, new_string,
				                              file, true, false));
			    });

			sec_tbl.set_function(
			    "remove",
			    [make_op](const std::string &section, const std::string &key,
			              const std::string &package,
			              sol::optional<std::string> file)
			    {
				    return sec_submit(
				        make_op(section, key, package, "", file, false, true));
			    });
		}

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
				    fn(gconfig, w.c_str(), nullptr);
			    });
		}

		tbl.set_function("on_map_load",
		                 [](sol::protected_function fn) -> bool
		                 {
			                 if (!fn.valid() || !ensure_map_hook())
				                 return false;
			                 g_map_cbs.push_back(std::move(fn));
			                 return true;
		                 });

		{
			sol::table obj_tbl = tbl["obj"].get_or_create<sol::table>();
			obj_tbl.set_function("path", [](void *o) { return path_of(o); });

			auto intern_leaf = [](const std::string &path) -> uint64_t
			{
				void *init = ue3().FNameInit;
				if (!init)
					return 0;
				const size_t dot = path.rfind('.');
				const std::string leaf =
				    (dot == std::string::npos) ? path : path.substr(dot + 1);
				FNameStack nm{};
				using InitFn = void(UE3_THISCALL *)(void *, const wchar_t *,
				                                    int, int, int);
				reinterpret_cast<InitFn>(init)(&nm, to_wide(leaf).c_str(), 0, 1,
				                               1);
				return name_key(nm.Index, nm.Number);
			};

			obj_tbl.set_function(
			    "on_preload",
			    [intern_leaf](const std::string &path,
			                  sol::protected_function fn,
			                  sol::optional<bool> verbose,
			                  sol::optional<bool> once) -> bool
			    {
				    if (!fn.valid())
					    return false;

				    const uint64_t key = intern_leaf(path);

				    if (!key)
				    {
					    log_warn("lua: on_preload — FName::Init unresolved");
					    return false;
				    }

				    PreloadWatch pw;
				    pw.path = lower_ascii(path);
				    pw.fn = std::move(fn);
				    pw.once = once.value_or(true);

				    std::lock_guard<std::mutex> lk(g_preload_mtx);
				    g_preload_watch[key].push_back(std::move(pw));
				    g_preload_live.fetch_add(1, std::memory_order_relaxed);
				    if (verbose)
					    log_info("lua: on_preload watching '%s' (key %llu)",
					             path.c_str(), (unsigned long long)key);
				    return true;
			    });

			obj_tbl.set_function(
			    "on_preload_many",
			    [intern_leaf](sol::table map, sol::protected_function fn,
			                  sol::optional<bool> once) -> int
			    {
				    if (!fn.valid())
					    return 0;
				    if (!ue3().FNameInit)
				    {
					    log_warn(
					        "lua: on_preload_many — FName::Init unresolved");
					    return 0;
				    }

				    const bool one_shot = once.value_or(true);
				    int added = 0;

				    std::lock_guard<std::mutex> lk(g_preload_mtx);
				    for (auto &kv : map)
				    {
					    if (kv.first.get_type() != sol::type::string)
						    continue;
					    const std::string path = kv.first.as<std::string>();
					    const uint64_t key = intern_leaf(path);
					    if (!key)
						    continue;

					    PreloadWatch pw;
					    pw.path = lower_ascii(path);
					    pw.fn = fn;
					    pw.payload = kv.second;
					    pw.once = one_shot;

					    g_preload_watch[key].push_back(std::move(pw));
					    ++added;
				    }
				    g_preload_live.fetch_add((size_t)added,
				                             std::memory_order_relaxed);
				    return added;
			    });
		}

		lua_lib::bind(lua);

		{
			sol::table msg_box = lua.create_named_table("msg_box");

			msg_box.new_enum(
			    "Icon", "None", msg_box::Icon::None, "Info",
			    msg_box::Icon::Info, "Warning", msg_box::Icon::Warning, "Error",
			    msg_box::Icon::Error, "Question", msg_box::Icon::Question);

			msg_box.new_usertype<msg_box::Result>(
			    "Result", sol::default_constructor, "closed",
			    &msg_box::Result::closed, "button_index",
			    &msg_box::Result::button_index, "button_label",
			    &msg_box::Result::button_label);

			msg_box.new_usertype<msg_box::Config>(
			    "Config", sol::default_constructor, "title",
			    &msg_box::Config::title, "message", &msg_box::Config::message,
			    "icon", &msg_box::Config::icon, "buttons",
			    sol::property([](msg_box::Config &cfg)
			                  { return sol::as_table(cfg.buttons); },
			                  [](msg_box::Config &cfg, sol::table t)
			                  {
				                  cfg.buttons.clear();
				                  for (auto &kv : t)
				                  {
					                  cfg.buttons.push_back(
					                      kv.second.as<std::string>());
				                  }
			                  }),
			    "default_button", &msg_box::Config::default_button,
			    "escape_button", &msg_box::Config::escape_button, "width",
			    &msg_box::Config::width);

			msg_box.set_function("show", &msg_box::show);
			msg_box.set_function("show_info", &msg_box::show_info);
			msg_box.set_function("show_warning", &msg_box::show_warning);
			msg_box.set_function("show_error", &msg_box::show_error);
			msg_box.set_function("show_confirm", &msg_box::show_confirm);
			msg_box.set_function("draw", &msg_box::draw);
			msg_box.set_function("poll", &msg_box::poll);
			msg_box.set_function("active", &msg_box::active);

			msg_box.set_function(
			    "make_config",
			    [](sol::table t)
			    {
				    msg_box::Config cfg;
				    cfg.title = t.get_or("title", cfg.title);
				    cfg.message = t.get_or("message", cfg.message);
				    cfg.icon = t.get_or("icon", cfg.icon);
				    if (t["buttons"].valid())
				    {
					    cfg.buttons.clear();
					    for (auto &kv : t["buttons"].get<sol::table>())
					    {
						    cfg.buttons.push_back(kv.second.as<std::string>());
					    }
				    }
				    cfg.default_button =
				        t.get_or("default_button", cfg.default_button);
				    cfg.escape_button =
				        t.get_or("escape_button", cfg.escape_button);
				    cfg.width = t.get_or("width", cfg.width);
				    return cfg;
			    });
		}

		log_info("lua: `ue3` table bound");
	}

	void run_startup_scripts()
	{
		for (const auto &lm : mod_loader::enabled_mods())
		{
			std::wstring path = lm.dir_w + L"\\main.lua";

			if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
			{
				std::string lua_dir = to_narrow(lm.dir_w);

				std::replace(lua_dir.begin(), lua_dir.end(), '\\', '/');

				sol::table package = (*g_lua)["package"];
				std::string old_path = package["path"];

				package["path"] = old_path + ";" + lua_dir + "/?.lua;" +
				                  lua_dir + "/?/init.lua";

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
			{
				g_nolua_switch = true;
				g_enabled = false;
			}
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

		g_enabled = loader_config::settings().lua && !g_nolua_switch;
		g_verbose = g_verbose || loader_config::settings().lua_verbose;

		if (!g_enabled)
		{
			log_info("lua: disabled");
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
		lua_lib::unload_all();
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

	void notify_preloaded(void *obj)
	{
		if (!obj || !g_lua)
			return;
		if (g_preload_live.load(std::memory_order_relaxed) == 0)
			return;

		const uint64_t key =
		    name_key(uobj_name_index(obj), uobj_name_number(obj));

		std::vector<std::pair<sol::protected_function, sol::object>> to_run;
		{
			std::lock_guard<std::mutex> lk(g_preload_mtx);

			auto it = g_preload_watch.find(key);
			if (it == g_preload_watch.end())
				return;

			const std::string path = lower_ascii(path_of(obj));
			if (path.empty())
				return;

			auto &vec = it->second;
			for (size_t k = 0; k < vec.size();)
			{
				auto &w = vec[k];
				if (!path_matches(path, w.path))
				{
					++k;
					continue;
				}
				if (w.once)
				{
					to_run.emplace_back(std::move(w.fn), std::move(w.payload));
					vec[k] = std::move(vec.back());
					vec.pop_back();
					g_preload_live.fetch_sub(1, std::memory_order_relaxed);
				}
				else
				{
					to_run.emplace_back(w.fn, w.payload);
					++k;
				}
			}
			if (vec.empty())
				g_preload_watch.erase(it);
		}

		for (auto &[fn, arg] : to_run)
		{
			auto r = arg.valid() ? fn(obj, arg) : fn(obj);
			if (!r.valid())
			{
				sol::error e = r;
				log_err("lua: on_preload callback: %s", e.what());
			}
		}
	}

}  // namespace lua_host
