#define SOL_ALL_SAFETIES_ON 1
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <sol/sol.hpp>

#include "lua_lib.hpp"

#include "logs.hpp"
#include "ue3_layout.hpp"
#include "util.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>

namespace
{
	using U = uintptr_t;

	constexpr int kMaxSlots = 16;

	template <class R, class Conv, std::size_t... I> struct CallGen;

#define CU3ML_DEFINE_CALLER(NAME, CONV)                                   \
	template <class R, std::size_t... I>                                  \
	R NAME##_n(void *fn, const U *a, std::index_sequence<I...>)           \
	{                                                                     \
		using Fn = R(CONV *)(decltype(I, U())...);                        \
		return reinterpret_cast<Fn>(fn)(a[I]...);                         \
	}                                                                     \
                                                                          \
	template <class R> bool NAME(void *fn, const U *a, int n, R &out)     \
	{                                                                     \
		switch (n)                                                        \
		{                                                                 \
			case 0:                                                       \
				out = NAME##_n<R>(fn, a, std::make_index_sequence<0>{});  \
				return true;                                              \
			case 1:                                                       \
				out = NAME##_n<R>(fn, a, std::make_index_sequence<1>{});  \
				return true;                                              \
			case 2:                                                       \
				out = NAME##_n<R>(fn, a, std::make_index_sequence<2>{});  \
				return true;                                              \
			case 3:                                                       \
				out = NAME##_n<R>(fn, a, std::make_index_sequence<3>{});  \
				return true;                                              \
			case 4:                                                       \
				out = NAME##_n<R>(fn, a, std::make_index_sequence<4>{});  \
				return true;                                              \
			case 5:                                                       \
				out = NAME##_n<R>(fn, a, std::make_index_sequence<5>{});  \
				return true;                                              \
			case 6:                                                       \
				out = NAME##_n<R>(fn, a, std::make_index_sequence<6>{});  \
				return true;                                              \
			case 7:                                                       \
				out = NAME##_n<R>(fn, a, std::make_index_sequence<7>{});  \
				return true;                                              \
			case 8:                                                       \
				out = NAME##_n<R>(fn, a, std::make_index_sequence<8>{});  \
				return true;                                              \
			case 9:                                                       \
				out = NAME##_n<R>(fn, a, std::make_index_sequence<9>{});  \
				return true;                                              \
			case 10:                                                      \
				out = NAME##_n<R>(fn, a, std::make_index_sequence<10>{}); \
				return true;                                              \
			case 11:                                                      \
				out = NAME##_n<R>(fn, a, std::make_index_sequence<11>{}); \
				return true;                                              \
			case 12:                                                      \
				out = NAME##_n<R>(fn, a, std::make_index_sequence<12>{}); \
				return true;                                              \
			case 13:                                                      \
				out = NAME##_n<R>(fn, a, std::make_index_sequence<13>{}); \
				return true;                                              \
			case 14:                                                      \
				out = NAME##_n<R>(fn, a, std::make_index_sequence<14>{}); \
				return true;                                              \
			case 15:                                                      \
				out = NAME##_n<R>(fn, a, std::make_index_sequence<15>{}); \
				return true;                                              \
			case 16:                                                      \
				out = NAME##_n<R>(fn, a, std::make_index_sequence<16>{}); \
				return true;                                              \
			default:                                                      \
				return false;                                             \
		}                                                                 \
	}

	CU3ML_DEFINE_CALLER(call_cdecl, __cdecl)

#ifndef _WIN64
	CU3ML_DEFINE_CALLER(call_stdcall, __stdcall)
	CU3ML_DEFINE_CALLER(call_thiscall, __thiscall)
#endif

#undef CU3ML_DEFINE_CALLER

	enum class Conv
	{
		Cdecl,
		Stdcall,
		Thiscall,
	};

	enum class Ty
	{
		Void,
		Int,
		UInt,
		I64,
		U64,
		Float,
		Double,
		Bool,
		Ptr,
		Str,
		WStr,
	};

	bool parse_type(const std::string &s, Ty &out)
	{
		static const std::unordered_map<std::string, Ty> kMap = {
		    {"void", Ty::Void}, {"int", Ty::Int},     {"i32", Ty::Int},
		    {"uint", Ty::UInt}, {"u32", Ty::UInt},    {"i64", Ty::I64},
		    {"u64", Ty::U64},   {"float", Ty::Float}, {"double", Ty::Double},
		    {"bool", Ty::Bool}, {"ptr", Ty::Ptr},     {"pointer", Ty::Ptr},
		    {"str", Ty::Str},   {"cstr", Ty::Str},    {"wstr", Ty::WStr},
		};
		auto it = kMap.find(s);
		if (it == kMap.end())
			return false;
		out = it->second;
		return true;
	}

	bool parse_conv(const std::string &s, Conv &out)
	{
		if (s.empty() || s == "cdecl" || s == "c")
		{
			out = Conv::Cdecl;
			return true;
		}
		if (s == "stdcall" || s == "std" || s == "winapi")
		{
			out = Conv::Stdcall;
			return true;
		}
		if (s == "thiscall" || s == "this")
		{
			out = Conv::Thiscall;
			return true;
		}
		return false;
	}

	struct Binding
	{
		std::string name;
		void *addr = nullptr;
		Ty ret = Ty::Void;
		std::vector<Ty> args;
		Conv conv = Conv::Cdecl;
	};

	struct Marshalled
	{
		U slots[kMaxSlots]{};
		int n = 0;
		std::vector<std::string> keep_a;
		std::vector<std::wstring> keep_w;
		std::string err;

		bool push(U v)
		{
			if (n >= kMaxSlots)
			{
				err = "too many argument slots";
				return false;
			}
			slots[n++] = v;
			return true;
		}
	};

	bool push_wide(Marshalled &m, uint64_t bits)
	{
#ifdef _WIN64
		return m.push((U)bits);
#else
		return m.push((U)(uint32_t)(bits & 0xffffffffull)) &&
		       m.push((U)(uint32_t)(bits >> 32));
#endif
	}

	bool marshal_one(Marshalled &m, Ty t, const sol::object &o)
	{
		switch (t)
		{
			case Ty::Bool:
				return m.push(o.as<bool>() ? 1u : 0u);

			case Ty::Int:
				return m.push((U)(int32_t)o.as<int64_t>());

			case Ty::UInt:
				return m.push((U)(uint32_t)o.as<int64_t>());

			case Ty::I64:
			case Ty::U64:
				return push_wide(m, (uint64_t)o.as<int64_t>());

			case Ty::Ptr:
				if (o.is<sol::lua_nil_t>())
					return m.push(0);
				if (o.is<void *>())
					return m.push((U)o.as<void *>());
				return m.push((U)o.as<uint64_t>());

			case Ty::Str:
			{
				m.keep_a.push_back(o.as<std::string>());
				return m.push((U)m.keep_a.back().c_str());
			}

			case Ty::WStr:
			{
				m.keep_w.push_back(to_wide(o.as<std::string>()));
				return m.push((U)m.keep_w.back().c_str());
			}

			case Ty::Float:
			{
#ifdef _WIN64
				m.err = "float/double arguments are not supported on x64 — use "
				        "CU3MLPluginInit, or pass a pointer to the value";
				return false;
#else
				const float f = (float)o.as<double>();
				uint32_t bits;
				memcpy(&bits, &f, 4);
				return m.push((U)bits);
#endif
			}

			case Ty::Double:
			{
#ifdef _WIN64
				m.err = "float/double arguments are not supported on x64 — use "
				        "CU3MLPluginInit, or pass a pointer to the value";
				return false;
#else
				const double d = o.as<double>();
				uint64_t bits;
				memcpy(&bits, &d, 8);
				return push_wide(m, bits);
#endif
			}

			default:
				m.err = "unsupported argument type";
				return false;
		}
	}

	template <class R>
	bool dispatch(Conv c, void *fn, const U *a, int n, R &out)
	{
		switch (c)
		{
			case Conv::Cdecl:
				return call_cdecl<R>(fn, a, n, out);
#ifndef _WIN64
			case Conv::Stdcall:
				return call_stdcall<R>(fn, a, n, out);
			case Conv::Thiscall:
				return call_thiscall<R>(fn, a, n, out);
#else
			case Conv::Stdcall:
			case Conv::Thiscall:
				return call_cdecl<R>(fn, a, n, out);
#endif
		}
		return false;
	}

	struct Lib
	{
		std::wstring path;
		HMODULE mod = nullptr;
		std::unordered_map<std::string, Binding> binds;
	};

	std::mutex g_mtx;
	std::vector<std::shared_ptr<Lib>> g_libs;
	std::unordered_map<std::string, std::pair<std::weak_ptr<Lib>, std::string>>
	    g_flat;

	void log_bridge_info(const char *m) { log_info("[plugin] %s", m); }

	void log_bridge_warn(const char *m) { log_warn("[plugin] %s", m); }

	void log_bridge_err(const char *m) { log_err("[plugin] %s", m); }

	CU3MLHostApi make_host_api()
	{
		CU3MLHostApi api{};
		api.size = (uint32_t)sizeof(CU3MLHostApi);
		api.abi = CU3ML_PLUGIN_ABI;
		api.log_info = &log_bridge_info;
		api.log_warn = &log_bridge_warn;
		api.log_err = &log_bridge_err;

		const UE3Layout &L = ue3();
		api.StaticLoadObject = L.StaticLoadObject;
		api.StaticFindObjectFast = L.StaticFindObjectFast;
		api.FNameInit = L.FNameInit;
		api.ArrayRealloc = L.ArrayRealloc;
		api.module_base = (void *)GetModuleHandleW(nullptr);
		return api;
	}

	std::wstring resolve_path(const std::string &p)
	{
		std::wstring w = to_wide(p);
		if (w.size() > 1 && (w[1] == L':' || w[0] == L'\\'))
			return w;
		return get_mods_dir() + L"\\" + w;
	}

	sol::object invoke(sol::this_state ts, const Binding &b,
	                   sol::variadic_args va)
	{
		sol::state_view lua(ts);

		const size_t want = b.args.size();
		if (va.size() != want)
		{
			log_err("lib: %s expects %zu arguments, got %zu", b.name.c_str(),
			        want, (size_t)va.size());
			return sol::lua_nil;
		}

		Marshalled m;
		for (size_t i = 0; i < want; ++i)
		{
			if (!marshal_one(m, b.args[i], va[i]))
			{
				log_err("lib: %s arg %zu: %s", b.name.c_str(), i + 1,
				        m.err.empty() ? "marshalling failed" : m.err.c_str());
				return sol::lua_nil;
			}
		}

		switch (b.ret)
		{
			case Ty::Void:
			{
				U r = 0;
				if (!dispatch<U>(b.conv, b.addr, m.slots, m.n, r))
					break;
				return sol::lua_nil;
			}
			case Ty::Bool:
			{
				U r = 0;
				if (!dispatch<U>(b.conv, b.addr, m.slots, m.n, r))
					break;
				return sol::make_object(lua, r != 0);
			}
			case Ty::Int:
			{
				U r = 0;
				if (!dispatch<U>(b.conv, b.addr, m.slots, m.n, r))
					break;
				return sol::make_object(lua, (int64_t)(int32_t)(uint32_t)r);
			}
			case Ty::UInt:
			{
				U r = 0;
				if (!dispatch<U>(b.conv, b.addr, m.slots, m.n, r))
					break;
				return sol::make_object(lua, (int64_t)(uint32_t)r);
			}
			case Ty::I64:
			case Ty::U64:
			{
				uint64_t r = 0;
				if (!dispatch<uint64_t>(b.conv, b.addr, m.slots, m.n, r))
					break;
				return sol::make_object(lua, (int64_t)r);
			}
			case Ty::Ptr:
			{
				U r = 0;
				if (!dispatch<U>(b.conv, b.addr, m.slots, m.n, r))
					break;
				if (!r)
					return sol::lua_nil;
				return sol::make_object(lua, (void *)r);
			}
			case Ty::Str:
			{
				U r = 0;
				if (!dispatch<U>(b.conv, b.addr, m.slots, m.n, r))
					break;
				const char *s = (const char *)r;
				if (!s || IsBadReadPtr(s, 1))
					return sol::lua_nil;
				return sol::make_object(lua, std::string(s));
			}
			case Ty::WStr:
			{
				U r = 0;
				if (!dispatch<U>(b.conv, b.addr, m.slots, m.n, r))
					break;
				const wchar_t *s = (const wchar_t *)r;
				if (!s || IsBadReadPtr(s, 2))
					return sol::lua_nil;
				return sol::make_object(lua, to_narrow(std::wstring(s)));
			}
			case Ty::Float:
			{
				float r = 0;
				if (!dispatch<float>(b.conv, b.addr, m.slots, m.n, r))
					break;
				return sol::make_object(lua, (double)r);
			}
			case Ty::Double:
			{
				double r = 0;
				if (!dispatch<double>(b.conv, b.addr, m.slots, m.n, r))
					break;
				return sol::make_object(lua, r);
			}
		}

		log_err("lib: %s — no dispatcher for %d slots", b.name.c_str(), m.n);
		return sol::lua_nil;
	}

	sol::object invoke(sol::this_state ts, const std::shared_ptr<Lib> &lib,
	                   const Binding &b, sol::variadic_args va)
	{
		(void)lib;
		return invoke(ts, b, va);
	}

	bool is_executable(const void *p)
	{
		MEMORY_BASIC_INFORMATION mbi{};
		if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)))
			return false;
		if (mbi.State != MEM_COMMIT)
			return false;
		const DWORD prot = mbi.Protect & 0xFF;
		return prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ ||
		       prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
	}

	void *resolve_addr(const sol::object &o)
	{
		void *p = nullptr;

		switch (o.get_type())
		{
			case sol::type::lightuserdata:
			case sol::type::userdata:
				p = o.as<void *>();
				break;
			case sol::type::number:
				p = (void *)(uintptr_t)o.as<uint64_t>();
				break;
			default:
				return nullptr;
		}

		if (!p)
			return nullptr;

		if (o.get_type() == sol::type::number && !is_executable(p))
		{
			auto *base = (uint8_t *)GetModuleHandleW(nullptr);
			void *as_rva = base + (uintptr_t)p;
			if (is_executable(as_rva))
			{
				log_info("lua: call_addr — treating 0x%zX as an RVA (%p)",
				         (size_t)(uintptr_t)p, as_rva);
				return as_rva;
			}
		}
		return p;
	}

	bool infer_type(const sol::object &o, Ty &out)
	{
		switch (o.get_type())
		{
			case sol::type::nil:
				out = Ty::Ptr;
				return true;
			case sol::type::boolean:
				out = Ty::Bool;
				return true;
			case sol::type::string:
				out = Ty::WStr;
				return true;
			case sol::type::lightuserdata:
			case sol::type::userdata:
				out = Ty::Ptr;
				return true;
			case sol::type::number:
			{
				const double d = o.as<double>();
				if (d == std::floor(d) && std::fabs(d) < 2147483648.0)
					out = Ty::Int;
				else
					out = Ty::Double;
				return true;
			}
			default:
				return false;
		}
	}

	bool build_binding(const sol::object &spec, sol::variadic_args va,
	                   Binding &b)
	{
		if (spec.get_type() == sol::type::table)
		{
			sol::table t = spec.as<sol::table>();

			b.addr = resolve_addr(t["addr"].get<sol::object>());
			if (!b.addr)
			{
				log_err("lua: call_addr — spec has no usable 'addr'");
				return false;
			}
			b.name = t.get_or<std::string>("name", "call_addr");

			if (!parse_type(t.get_or<std::string>("ret", "int"), b.ret))
			{
				log_err("lua: call_addr — bad return type");
				return false;
			}
			if (!parse_conv(t.get_or<std::string>("conv", "cdecl"), b.conv))
			{
				log_err("lua: call_addr — bad calling convention");
				return false;
			}

			sol::optional<sol::table> args = t["args"];
			if (args)
			{
				for (std::size_t i = 1; i <= args->size(); ++i)
				{
					Ty ty;
					if (!parse_type(args->get<std::string>(i), ty))
					{
						log_err("lua: call_addr — bad arg type at %zu", i);
						return false;
					}
					b.args.push_back(ty);
				}
				return true;
			}
		}
		else
		{
			b.addr = resolve_addr(spec);
			if (!b.addr)
			{
				log_err("lua: call_addr — first argument is not an address");
				return false;
			}
			b.name = "call_addr";
			b.ret = Ty::Int;
			b.conv = Conv::Cdecl;
		}

		for (std::size_t i = 0; i < va.size(); ++i)
		{
			Ty ty;
			if (!infer_type(va[i], ty))
			{
				log_err("lua: call_addr — cannot infer a type for argument %zu",
				        i + 1);
				return false;
			}
			b.args.push_back(ty);
		}
		return true;
	}
}  // namespace

namespace lua_lib
{
	void bind(sol::state &lua)
	{
		sol::usertype<Lib> ut = lua.new_usertype<Lib>(
		    "CU3MLLib", sol::no_constructor,

		    "path", sol::property([](Lib &l) { return to_narrow(l.path); }),

		    "has", [](Lib &l, const std::string &sym)
		    { return l.mod && GetProcAddress(l.mod, sym.c_str()) != nullptr; },

		    "addr",
		    [](Lib &l, const std::string &sym) -> sol::optional<void *>
		    {
			    if (!l.mod)
				    return sol::nullopt;
			    void *p = (void *)GetProcAddress(l.mod, sym.c_str());
			    if (!p)
				    return sol::nullopt;
			    return p;
		    });

		sol::table tbl = lua["lib"].get_or_create<sol::table>();

		tbl.set_function(
		    "load",
		    [&lua](const std::string &path) -> sol::object
		    {
			    const std::wstring full = resolve_path(path);

			    HMODULE h = LoadLibraryExW(full.c_str(), nullptr,
			                               LOAD_WITH_ALTERED_SEARCH_PATH);
			    if (!h)
			    {
				    log_err("lib: LoadLibrary('%ls') failed (err=%lu)",
				            full.c_str(), GetLastError());
				    return sol::lua_nil;
			    }

			    auto lp = std::make_shared<Lib>();
			    lp->path = full;
			    lp->mod = h;
			    {
				    std::lock_guard<std::mutex> lk(g_mtx);
				    g_libs.push_back(lp);
			    }
			    log_info("lib: loaded %ls (base=%p)", full.c_str(), (void *)h);

			    if (auto init =
			            (CU3MLPluginInitFn)GetProcAddress(h, "CU3MLPluginInit"))
			    {
				    CU3MLHostApi api = make_host_api();
				    int rc = 0;
				    try
				    {
					    rc = init(lua.lua_state(), &api);
				    }
				    catch (const std::exception &e)
				    {
					    log_err("lib: CU3MLPluginInit threw: %s", e.what());
					    rc = -1;
				    }
				    catch (...)
				    {
					    log_err("lib: CU3MLPluginInit threw");
					    rc = -1;
				    }
				    log_info("lib: CU3MLPluginInit -> %d", rc);
			    }

			    return sol::make_object(lua, lp);
		    });

		tbl.set_function(
		    "bind",
		    [](std::shared_ptr<Lib> lp, const std::string &name,
		       sol::table spec) -> bool
		    {
			    if (!lp || !lp->mod)
				    return false;

			    const std::string symbol =
			        spec.get_or<std::string>("symbol", name);

			    void *addr = (void *)GetProcAddress(lp->mod, symbol.c_str());
			    if (!addr)
			    {
				    log_err("lib: '%s' not exported by %ls", symbol.c_str(),
				            lp->path.c_str());
				    return false;
			    }

			    Binding b;
			    b.name = name;
			    b.addr = addr;

			    if (!parse_type(spec.get_or<std::string>("ret", "void"), b.ret))
			    {
				    log_err("lib: bind %s — bad return type", name.c_str());
				    return false;
			    }
			    if (!parse_conv(spec.get_or<std::string>("conv", "cdecl"),
			                    b.conv))
			    {
				    log_err("lib: bind %s — bad calling convention",
				            name.c_str());
				    return false;
			    }

			    sol::optional<sol::table> args = spec["args"];
			    if (args)
			    {
				    for (std::size_t i = 1; i <= args->size(); ++i)
				    {
					    Ty t;
					    if (!parse_type(args->get<std::string>(i), t))
					    {
						    log_err("lib: bind %s — bad arg type at %zu",
						            name.c_str(), i);
						    return false;
					    }
					    b.args.push_back(t);
				    }
			    }

			    std::lock_guard<std::mutex> lk(g_mtx);
			    lp->binds[name] = b;
			    g_flat[name] = {std::weak_ptr<Lib>(lp), name};
			    log_info("lib: bound %s -> %p (%zu args)", name.c_str(), addr,
			             b.args.size());
			    return true;
		    });

		tbl.set_function(
		    "call",
		    [](sol::this_state ts, sol::object first,
		       sol::variadic_args va) -> sol::object
		    {
			    std::shared_ptr<Lib> lp;
			    std::string name;
			    sol::variadic_args rest = va;

			    if (first.is<std::shared_ptr<Lib>>())
			    {
				    lp = first.as<std::shared_ptr<Lib>>();
				    if (va.size() == 0)
					    return sol::lua_nil;
				    name = va[0].as<std::string>();
				    rest = sol::variadic_args(va.lua_state(),
				                              va.stack_index() + 1);
			    }
			    else
			    {
				    name = first.as<std::string>();
				    std::lock_guard<std::mutex> lk(g_mtx);
				    auto it = g_flat.find(name);
				    if (it == g_flat.end())
				    {
					    log_err("lib: '%s' is not bound", name.c_str());
					    return sol::lua_nil;
				    }
				    lp = it->second.first.lock();
			    }

			    if (!lp)
			    {
				    log_err("lib: '%s' — owning library was unloaded",
				            name.c_str());
				    return sol::lua_nil;
			    }

			    Binding b;
			    {
				    std::lock_guard<std::mutex> lk(g_mtx);
				    auto it = lp->binds.find(name);
				    if (it == lp->binds.end())
				    {
					    log_err("lib: '%s' is not bound in %ls", name.c_str(),
					            lp->path.c_str());
					    return sol::lua_nil;
				    }
				    b = it->second;
			    }

			    return invoke(ts, lp, b, rest);
		    });

		tbl.set_function(
		    "unload",
		    [](std::shared_ptr<Lib> lp)
		    {
			    if (!lp || !lp->mod)
				    return false;
			    std::lock_guard<std::mutex> lk(g_mtx);
			    for (auto it = g_flat.begin(); it != g_flat.end();)
			    {
				    auto owner = it->second.first.lock();
				    it = (owner == lp) ? g_flat.erase(it) : std::next(it);
			    }
			    FreeLibrary(lp->mod);
			    log_info("lib: unloaded %ls", lp->path.c_str());
			    lp->mod = nullptr;
			    lp->binds.clear();
			    g_libs.erase(std::remove(g_libs.begin(), g_libs.end(), lp),
			                 g_libs.end());
			    return true;
		    });

		tbl.set_function("loaded",
		                 [&lua]()
		                 {
			                 sol::table t = lua.create_table();
			                 std::lock_guard<std::mutex> lk(g_mtx);
			                 int i = 1;
			                 for (auto &l : g_libs)
				                 t[i++] = to_narrow(l->path);
			                 return t;
		                 });

		lua["ue3"]["lib"] = tbl;

		sol::table ue3_tbl = lua["ue3"].get_or_create<sol::table>();

		ue3_tbl.set_function(
		    "rva", [](uintptr_t off) -> void *
		    { return (uint8_t *)GetModuleHandleW(nullptr) + off; });

		ue3_tbl.set_function(
		    "call_addr",
		    [](sol::this_state ts, sol::object spec,
		       sol::variadic_args va) -> sol::object
		    {
			    Binding b;
			    if (!build_binding(spec, va, b))
				    return sol::lua_nil;

			    if (!is_executable(b.addr))
			    {
				    log_err("lua: call_addr — %p is not executable memory",
				            b.addr);
				    return sol::lua_nil;
			    }
			    return invoke(ts, b, va);
		    });

		log_info("lua: `lib` table bound");
	}

	void unload_all()
	{
		std::lock_guard<std::mutex> lk(g_mtx);
		g_flat.clear();
		for (auto &l : g_libs)
			if (l->mod)
			{
				FreeLibrary(l->mod);
				l->mod = nullptr;
			}
		g_libs.clear();
	}
}  // namespace lua_lib
