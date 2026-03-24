#include "Runtime.hpp"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <v8.h>
#include <libplatform/libplatform.h>
#if defined(V8_ENABLE_SANDBOX)
#include <v8-sandbox.h>
#endif

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <dlfcn.h>
#include <unistd.h>

#ifndef MLXF_RUNTIME_INCLUDE_DIR
#define MLXF_RUNTIME_INCLUDE_DIR "."
#endif
#ifndef NLOHMANN_JSON_INCLUDE_DIR
#define NLOHMANN_JSON_INCLUDE_DIR "."
#endif

namespace fs = std::filesystem;
namespace py = pybind11;

namespace {

constexpr const char* kReset = "\033[0m";
constexpr const char* kGreen = "\033[32m";
constexpr const char* kMagenta = "\033[35m";
constexpr const char* kBlue = "\033[34m";
constexpr const char* kRed = "\033[31m";
constexpr const char* kYellow = "\033[33m";

struct Options {
    bool verbose = false;
    bool no_cpp_compile = false;
    bool cleanup_temps = true;
    std::string poly_path;
};

void log_section(const char* section, const char* color) {
    std::cerr << color << "[mlxf] " << kReset << "Loaded section /" << section << std::endl;
}

std::string trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return std::string(s);
}

bool is_section_header(std::string_view line, std::string& out_name) {
    line = trim(std::string(line));
    if (line.empty() || line[0] != '/') {
        return false;
    }
    if (line.size() < 2) {
        return false;
    }
    std::string_view rest = line.substr(1);
    if (rest == "cpp") {
        out_name = "cpp";
        return true;
    }
    if (rest == "py") {
        out_name = "py";
        return true;
    }
    if (rest == "js") {
        out_name = "js";
        return true;
    }
    if (rest == "main") {
        out_name = "main";
        return true;
    }
    return false;
}

std::unordered_map<std::string, std::string> parse_poly_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("Cannot open .poly file: " + path);
    }
    std::unordered_map<std::string, std::string> sections;
    std::string current;
    std::ostringstream buf;
    std::string line;
    while (std::getline(in, line)) {
        std::string sec;
        if (is_section_header(line, sec)) {
            if (!current.empty()) {
                sections[current] = buf.str();
                buf.str("");
                buf.clear();
            }
            current = sec;
            continue;
        }
        if (!current.empty()) {
            buf << line << '\n';
        }
    }
    if (!current.empty()) {
        sections[current] = buf.str();
    }
    return sections;
}

nlohmann::json py_object_to_json(const py::object& o) {
    if (o.is_none()) {
        return nlohmann::json::object();
    }
    py::object dumps = py::module_::import("json").attr("dumps");
    std::string s = py::cast<std::string>(dumps(o));
    return nlohmann::json::parse(s);
}

py::object json_to_py_object(const nlohmann::json& j) {
    py::object loads = py::module_::import("json").attr("loads");
    return loads(j.dump());
}

std::unordered_map<std::string, std::string> autobind_sections_with_treesitter(
    const std::unordered_map<std::string, std::string>& sections, bool verbose) {
    py::dict globals;
    globals["py_code"] = py::str(sections.count("py") ? sections.at("py") : "");
    globals["js_code"] = py::str(sections.count("js") ? sections.at("js") : "");
    globals["cpp_code"] = py::str(sections.count("cpp") ? sections.at("cpp") : "");

    // Uses tree-sitter + tree_sitter_languages to discover function declarations and
    // emits binding code so users do not manually call runtime.register in each section.
    py::exec(R"PY(
import json

try:
    from tree_sitter import Parser, Language
except Exception as exc:
    raise RuntimeError(
        "Tree-sitter auto-bind requires Python package 'tree_sitter'. Install "
        "it (for example: pip install tree_sitter)."
    ) from exc

_LANG_CACHE = {}

def _load_lang(name):
    if name in _LANG_CACHE:
        return _LANG_CACHE[name]
    # Preferred path when available.
    try:
        from tree_sitter_languages import get_language  # type: ignore
        lang = get_language(name)
        _LANG_CACHE[name] = lang
        return lang
    except Exception:
        pass
    # Fallback for Python 3.13-friendly split packages.
    mod_name = {
        "python": "tree_sitter_python",
        "javascript": "tree_sitter_javascript",
        "cpp": "tree_sitter_cpp",
    }.get(name)
    if not mod_name:
        raise RuntimeError(f"Unsupported language for auto-bind: {name}")
    try:
        mod = __import__(mod_name)
        lang = Language(mod.language())
        _LANG_CACHE[name] = lang
        return lang
    except Exception as exc:
        raise RuntimeError(
            "Tree-sitter language module missing for '" + name + "'. "
            "Install either 'tree_sitter_languages' or split packages: "
            "tree_sitter_python tree_sitter_javascript tree_sitter_cpp."
        ) from exc

def _mk_parser(lang_name):
    lang = _load_lang(lang_name)
    parser = Parser(lang)
    return parser

def _node_text(code_b, node):
    return code_b[node.start_byte:node.end_byte].decode("utf-8")

def _ident_from_param_text(param_text):
    s = param_text.strip()
    if not s:
        return None
    # Drop defaults and basic punct.
    if "=" in s:
        s = s.split("=", 1)[0].strip()
    s = s.lstrip("*&").strip()
    parts = [p for p in s.replace("\n", " ").split(" ") if p]
    if not parts:
        return None
    last = parts[-1].strip()
    last = last.strip("*,&")
    if not last:
        return None
    # Skip obvious non-identifiers.
    if any(ch in last for ch in "[]()<>:"):
        return None
    return last

def parse_python_functions(code):
    if not code.strip():
        return []
    parser = _mk_parser("python")
    code_b = code.encode("utf-8")
    tree = parser.parse(code_b)
    root = tree.root_node
    out = []
    for n in root.children:
        if n.type != "function_definition":
            continue
        name = None
        params = []
        for c in n.children:
            if c.type == "identifier":
                name = _node_text(code_b, c)
            elif c.type == "parameters":
                txt = _node_text(code_b, c)
                if txt.startswith("(") and txt.endswith(")"):
                    txt = txt[1:-1]
                raw = [p.strip() for p in txt.split(",") if p.strip()]
                for p in raw:
                    if p.startswith("*"):
                        continue
                    if "=" in p:
                        p = p.split("=", 1)[0].strip()
                    if ":" in p:
                        p = p.split(":", 1)[0].strip()
                    if p:
                        params.append(p)
        if name:
            out.append({"name": name, "params": params})
    return out

def parse_js_functions(code):
    if not code.strip():
        return []
    parser = _mk_parser("javascript")
    code_b = code.encode("utf-8")
    tree = parser.parse(code_b)
    root = tree.root_node
    out = []
    def walk(node):
        if node.type == "function_declaration":
            name = None
            params = []
            for c in node.children:
                if c.type == "identifier" and name is None:
                    name = _node_text(code_b, c)
                elif c.type == "formal_parameters":
                    txt = _node_text(code_b, c)
                    if txt.startswith("(") and txt.endswith(")"):
                        txt = txt[1:-1]
                    params = [p.strip() for p in txt.split(",") if p.strip()]
            if name:
                out.append({"name": name, "params": params})
        for ch in node.children:
            walk(ch)
    walk(root)
    return out

def _parse_cpp_param_decl(param_text):
    p = param_text.strip()
    if not p or p == "void":
        return None
    if "=" in p:
        p = p.split("=", 1)[0].strip()
    chunks = p.replace("\n", " ").split()
    if len(chunks) < 2:
        return None
    name = chunks[-1].strip("&*")
    ctype = " ".join(chunks[:-1]).strip()
    if not name or not ctype:
        return None
    return {"name": name, "ctype": ctype}

def parse_cpp_json_functions(code):
    if not code.strip():
        return {"init_runtime_present": False, "functions": []}
    parser = _mk_parser("cpp")
    code_b = code.encode("utf-8")
    tree = parser.parse(code_b)
    root = tree.root_node
    init_runtime_present = False
    fns = []
    def walk(node):
        nonlocal init_runtime_present
        if node.type == "function_definition":
            txt = _node_text(code_b, node)
            head = txt.split("{", 1)[0]
            if "init_runtime" in head:
                init_runtime_present = True
            if "init_runtime" in head:
                return
            if "(" not in head or ")" not in head:
                return
            before_paren = head.split("(", 1)[0].strip()
            ret = " ".join(before_paren.split()[:-1]).strip()
            name = before_paren.split()[-1].strip("*&")
            if not name or name in ("if", "for", "while", "switch"):
                return
            if "::" in name:
                return
            params_txt = head.split("(", 1)[1].rsplit(")", 1)[0]
            params = [p.strip() for p in params_txt.split(",") if p.strip()]
            parsed_params = []
            for p in params:
                parsed = _parse_cpp_param_decl(p)
                if not parsed:
                    return
                parsed_params.append(parsed)
            fns.append({"name": name, "return_type": ret, "params": parsed_params})
        for ch in node.children:
            walk(ch)
    walk(root)
    dedup = []
    seen = set()
    for f in fns:
        if f["name"] in seen:
            continue
        seen.add(f["name"])
        dedup.append(f)
    return {"init_runtime_present": init_runtime_present, "functions": dedup}

def build_py_autobind(py_code, fns):
    if not fns:
        return py_code
    lines = []
    lines.append("")
    lines.append("# --- mlxf auto-bind (generated via tree-sitter) ---")
    lines.append("def __mlxf__as_dict(v, param_names=None):")
    lines.append("    if v is None:")
    lines.append("        return {}")
    lines.append("    if isinstance(v, dict):")
    lines.append("        return v")
    lines.append("    if isinstance(v, (list, tuple)):")
    lines.append("        names = list(param_names or [])")
    lines.append("        return { (names[i] if i < len(names) else f'arg{i}'): v[i] for i in range(len(v)) }")
    lines.append("    raise TypeError('runtime.call args must be a dict/object/array')")
    for f in fns:
        name = f["name"]
        params = f.get("params", [])
        lines.append(f"def __mlxf__wrap__{name}(__mlxf_args):")
        lines.append(f"    __mlxf_params = {repr(params)}")
        lines.append("    __mlxf_d = __mlxf__as_dict(__mlxf_args, __mlxf_params)")
        if len(params) == 1 and params[0] in ("args", "payload", "data"):
            lines.append(f"    return {name}(__mlxf_d)")
        elif len(params) == 0:
            lines.append(f"    return {name}()")
        else:
            lines.append(f"    return {name}(**__mlxf_d)")
        lines.append(f"runtime.register('{name}', __mlxf__wrap__{name})")
    return py_code + "\n" + "\n".join(lines) + "\n"

def build_js_autobind(js_code, fns):
    if not fns:
        return js_code
    lines = []
    lines.append("")
    lines.append("// --- mlxf auto-bind (generated via tree-sitter) ---")
    lines.append("function __mlxf__asObject(v, paramNames) {")
    lines.append("  if (v === undefined || v === null) return {};")
    lines.append("  if (Array.isArray(v)) {")
    lines.append("    const out = {};")
    lines.append("    const names = paramNames || [];")
    lines.append("    for (let i = 0; i < v.length; i += 1) {")
    lines.append("      out[(i < names.length) ? names[i] : `arg${i}`] = v[i];")
    lines.append("    }")
    lines.append("    return out;")
    lines.append("  }")
    lines.append("  if (typeof v !== 'object') {")
    lines.append("    throw new TypeError('runtime.call args must be an object');")
    lines.append("  }")
    lines.append("  return v;")
    lines.append("}")
    for f in fns:
        name = f["name"]
        params = f.get("params", [])
        lines.append(f"runtime.register('{name}', function(__mlxf_args) {{")
        lines.append(f"  const __mlxf_params = {json.dumps(params)};")
        lines.append("  const __mlxf_d = __mlxf__asObject(__mlxf_args, __mlxf_params);")
        if len(params) == 1 and params[0] in ("args", "payload", "data"):
            lines.append(f"  return {name}(__mlxf_d);")
        elif len(params) == 0:
            lines.append(f"  return {name}();")
        else:
            param_list = ", ".join([f"__mlxf_d[{json.dumps(p)}]" for p in params])
            lines.append(f"  return {name}({param_list});")
        lines.append("});")
    return js_code + "\n" + "\n".join(lines) + "\n"

def build_cpp_autobind(cpp_code, cpp_meta):
    if not cpp_code.strip():
        return cpp_code
    if cpp_meta.get("init_runtime_present", False):
        return cpp_code
    fns = cpp_meta.get("functions", [])
    if not fns:
        return cpp_code
    lines = []
    lines.append("")
    lines.append("// --- mlxf auto-bind (generated via tree-sitter) ---")
    lines.append('extern "C" void init_runtime(void* rt_ptr) {')
    lines.append("    auto* rt = static_cast<Runtime*>(rt_ptr);")
    lines.append("    auto __mlxf_get_arg = [](const nlohmann::json& args, const std::string& key, size_t idx) -> nlohmann::json {")
    lines.append("        if (args.is_object() && args.contains(key)) return args.at(key);")
    lines.append("        if (args.is_array() && idx < args.size()) return args.at(idx);")
    lines.append("        return nlohmann::json();")
    lines.append("    };")
    for f in fns:
        name = f["name"]
        ret = f.get("return_type", "")
        params = f.get("params", [])
        lines.append(f'    rt->register_function("{name}", [&](const nlohmann::json& __mlxf_in) -> nlohmann::json {{')
        arg_names = []
        for i, p in enumerate(params):
            pname = p["name"]
            ptype = p["ctype"]
            arg_names.append(pname)
            pnorm = ptype.replace("const", "").replace("&", "").replace("*", "").strip()
            if pnorm == "nlohmann::json" or pnorm == "json":
                if len(params) == 1:
                    lines.append(f'        {ptype} {pname} = __mlxf_in;')
                else:
                    lines.append(f'        auto __mlxf_v{i} = __mlxf_get_arg(__mlxf_in, "{pname}", {i});')
                    lines.append(f'        {ptype} {pname} = __mlxf_v{i};')
            elif pnorm in ("std::string", "string"):
                lines.append(f'        auto __mlxf_v{i} = __mlxf_get_arg(__mlxf_in, "{pname}", {i});')
                lines.append(f'        {ptype} {pname} = __mlxf_v{i}.is_null() ? std::string{{}} : __mlxf_v{i}.get<std::string>();')
            else:
                lines.append(f'        auto __mlxf_v{i} = __mlxf_get_arg(__mlxf_in, "{pname}", {i});')
                lines.append(f'        {ptype} {pname} = __mlxf_v{i}.is_null() ? {ptype}{{}} : __mlxf_v{i}.get<{ptype}>();')
        call_expr = f"{name}(" + ", ".join(arg_names) + ")"
        if "void" == ret.strip():
            lines.append(f"        {call_expr};")
            lines.append("        return nlohmann::json::object();")
        elif "json" in ret:
            lines.append(f"        return {call_expr};")
        else:
            lines.append(f"        return nlohmann::json({call_expr});")
        lines.append("    });")
    lines.append("}")
    return cpp_code + "\n" + "\n".join(lines) + "\n"

py_funcs = parse_python_functions(py_code)
js_funcs = parse_js_functions(js_code)
cpp_meta = parse_cpp_json_functions(cpp_code)

result = {
    "py": build_py_autobind(py_code, py_funcs),
    "js": build_js_autobind(js_code, js_funcs),
    "cpp": build_cpp_autobind(cpp_code, cpp_meta),
}

autobind_result_json = json.dumps(result)
)PY",
             globals, globals);

    py::object json_result_obj = globals["autobind_result_json"];
    nlohmann::json parsed = nlohmann::json::parse(py::cast<std::string>(json_result_obj));
    std::unordered_map<std::string, std::string> out = sections;
    out["py"] = parsed.value("py", sections.count("py") ? sections.at("py") : "");
    out["js"] = parsed.value("js", sections.count("js") ? sections.at("js") : "");
    out["cpp"] = parsed.value("cpp", sections.count("cpp") ? sections.at("cpp") : "");

    if (verbose) {
        std::cerr << kBlue << "[mlxf] " << kReset
                  << "Auto-bind enabled (tree-sitter metadata extracted)." << std::endl;
    }
    return out;
}

struct V8Host;

struct JsRuntimeData {
    Runtime* runtime = nullptr;
    V8Host* host = nullptr;
};

void register_runtime_pybind_once() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    py::module_ main = py::module_::import("__main__");
    py::class_<Runtime>(main, "Runtime")
        .def(
            "register",
            [](Runtime& r, const std::string& name, py::object fn) {
                r.register_function(name, [fn](const nlohmann::json& args) -> nlohmann::json {
                    py::gil_scoped_acquire gil;
                    py::object py_args = json_to_py_object(args);
                    py::object res = fn(py_args);
                    if (res.is_none()) {
                        return nlohmann::json::object();
                    }
                    return py_object_to_json(res);
                });
            },
            py::arg("name"), py::arg("callable"))
        .def(
            "call",
            [](Runtime& r, const std::string& name, py::object args) {
                nlohmann::json jargs = args.is_none() ? nlohmann::json::object()
                                                      : py_object_to_json(args);
                nlohmann::json out = r.call(name, jargs);
                if (out.contains("__error__") && out["__error__"].get<bool>()) {
                    throw std::runtime_error(out.value("message", "call failed"));
                }
                return json_to_py_object(out);
            },
            py::arg("name"), py::arg("args") = py::none());
}

struct V8Host {
    std::unique_ptr<v8::Platform> platform;
    v8::Isolate* isolate = nullptr;
    v8::Global<v8::Context> context_global;
    v8::ArrayBuffer::Allocator* allocator = nullptr;
    std::unique_ptr<JsRuntimeData> js_data;

    ~V8Host() {
        if (isolate) {
            context_global.Reset();
            isolate->Dispose();
            isolate = nullptr;
        }
        if (allocator) {
            delete allocator;
            allocator = nullptr;
        }
        if (platform) {
            v8::V8::Dispose();
            v8::V8::DisposePlatform();
            platform.reset();
        }
    }
};

v8::Local<v8::Value> json_to_v8_value(v8::Isolate* isolate, v8::Local<v8::Context> ctx,
                                       const nlohmann::json& j) {
    v8::Local<v8::String> s =
        v8::String::NewFromUtf8(isolate, j.dump().c_str(),
                                 v8::NewStringType::kNormal)
            .ToLocalChecked();
    v8::MaybeLocal<v8::Value> parsed = v8::JSON::Parse(ctx, s);
    v8::Local<v8::Value> val;
    if (!parsed.ToLocal(&val)) {
        return v8::Undefined(isolate);
    }
    return val;
}

nlohmann::json v8_value_to_json(v8::Isolate* isolate, v8::Local<v8::Context> ctx,
                                 v8::Local<v8::Value> val) {
    v8::MaybeLocal<v8::String> maybe = v8::JSON::Stringify(ctx, val);
    v8::Local<v8::String> str;
    if (!maybe.ToLocal(&str)) {
        return nlohmann::json::object();
    }
    v8::String::Utf8Value utf8(isolate, str);
    return nlohmann::json::parse(std::string(*utf8, utf8.length()));
}

static void JsRegisterCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* isolate = args.GetIsolate();
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    auto* data = static_cast<JsRuntimeData*>(
        v8::Local<v8::External>::Cast(args.Data())->Value());
    if (args.Length() < 2 || !args[0]->IsString() || !args[1]->IsFunction()) {
        isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8(isolate,
                                      "register(name, fn) requires a string and a function",
                                      v8::NewStringType::kNormal)
                .ToLocalChecked()));
        return;
    }
    v8::String::Utf8Value name_utf8(isolate, args[0]);
    std::string name_str(*name_utf8 ? *name_utf8 : "");
    v8::Local<v8::Function> fn = args[1].As<v8::Function>();
    auto g_fn = std::make_shared<v8::Global<v8::Function>>(isolate, fn);
    Runtime* runtime = data->runtime;
    V8Host* hp = data->host;
    runtime->register_function(name_str, [isolate, hp, g_fn](const nlohmann::json& jargs) -> nlohmann::json {
        v8::Locker locker(isolate);
        v8::Isolate::Scope isolate_scope(isolate);
        v8::HandleScope hs(isolate);
        v8::Local<v8::Context> ctx = hp->context_global.Get(isolate);
        v8::Context::Scope cs(ctx);
        v8::Local<v8::Function> local_fn = g_fn->Get(isolate);
        v8::Local<v8::Value> argv[1] = {json_to_v8_value(isolate, ctx, jargs)};
        v8::Local<v8::Value> recv = ctx->Global();
        v8::TryCatch try_catch(isolate);
        v8::MaybeLocal<v8::Value> maybe = local_fn->Call(ctx, recv, 1, argv);
        if (maybe.IsEmpty()) {
            if (try_catch.HasCaught()) {
                v8::String::Utf8Value err(isolate, try_catch.Exception());
                return nlohmann::json{{"__error__", true},
                                      {"message", std::string(*err ? *err : "")}};
            }
            return nlohmann::json::object();
        }
        return v8_value_to_json(isolate, ctx, maybe.ToLocalChecked());
    });
}

static void JsCallCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* isolate = args.GetIsolate();
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    auto* data = static_cast<JsRuntimeData*>(
        v8::Local<v8::External>::Cast(args.Data())->Value());
    if (args.Length() < 1 || !args[0]->IsString()) {
        isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8(isolate, "call(name, args?) requires a string name",
                                      v8::NewStringType::kNormal)
                .ToLocalChecked()));
        return;
    }
    v8::String::Utf8Value name_utf8(isolate, args[0]);
    std::string name_str(*name_utf8 ? *name_utf8 : "");
    nlohmann::json jargs = nlohmann::json::object();
    if (args.Length() >= 2 && !args[1]->IsUndefined()) {
        jargs = v8_value_to_json(isolate, context, args[1]);
    }
    nlohmann::json out = data->runtime->call(name_str, jargs);
    args.GetReturnValue().Set(json_to_v8_value(isolate, context, out));
}

void run_python_section(Runtime& runtime, const std::string& code, bool verbose) {
    if (code.empty()) {
        return;
    }
    log_section("py", kGreen);
    if (verbose) {
        std::cerr << kBlue << "--- Python (/py) ---" << kReset << "\n"
                  << code << std::endl;
    }
    py::dict global = py::module_::import("__main__").attr("__dict__");
    global["runtime"] = py::cast(&runtime, py::return_value_policy::reference);
    py::exec(code, global, global);
}

void run_js_section(Runtime& runtime, const std::string& code, bool verbose,
                    std::unique_ptr<V8Host>& host_out, const char* exec_path) {
    if (code.empty()) {
        return;
    }
    log_section("js", kMagenta);
    if (verbose) {
        std::cerr << kBlue << "--- JavaScript (/js) ---" << kReset << "\n"
                  << code << std::endl;
    }

    host_out = std::make_unique<V8Host>();
    V8Host& host = *host_out;

    // Platform + V8::Initialize first (initializes sandbox when V8_ENABLE_SANDBOX).
    // Do not call ArrayBuffer::Allocator::NewDefaultAllocator() before Initialize — it
    // uses the sandbox page allocator when V8_ENABLE_SANDBOX is set (see gdb backtrace).
    host.platform = v8::platform::NewDefaultPlatform();
    v8::V8::InitializePlatform(host.platform.get());
    v8::V8::Initialize();

    if (exec_path && exec_path[0] != '\0') {
        v8::V8::InitializeICUDefaultLocation(exec_path);
        v8::V8::InitializeExternalStartupData(exec_path);
    }

    host.allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();

    v8::Isolate::CreateParams params;
    params.array_buffer_allocator = host.allocator;
    host.isolate = v8::Isolate::New(params);

    v8::Isolate* isolate = host.isolate;
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);

    v8::Local<v8::ObjectTemplate> global_t = v8::ObjectTemplate::New(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate, nullptr, global_t);
    host.context_global.Reset(isolate, context);

    v8::Context::Scope context_scope(context);

    host_out->js_data = std::make_unique<JsRuntimeData>();
    host_out->js_data->runtime = &runtime;
    host_out->js_data->host = host_out.get();

    v8::Local<v8::External> ext = v8::External::New(isolate, host_out->js_data.get());
    v8::Local<v8::FunctionTemplate> register_fn =
        v8::FunctionTemplate::New(isolate, JsRegisterCallback, ext);
    v8::Local<v8::FunctionTemplate> call_fn =
        v8::FunctionTemplate::New(isolate, JsCallCallback, ext);

    v8::Local<v8::ObjectTemplate> rt_tpl = v8::ObjectTemplate::New(isolate);
    rt_tpl->Set(isolate, "register", register_fn);
    rt_tpl->Set(isolate, "call", call_fn);

    v8::Local<v8::Object> rt_obj = rt_tpl->NewInstance(context).ToLocalChecked();
    v8::Local<v8::String> runtime_key =
        v8::String::NewFromUtf8(isolate, "runtime", v8::NewStringType::kInternalized)
            .ToLocalChecked();
    context->Global()->Set(context, runtime_key, rt_obj).Check();

    v8::Local<v8::String> src =
        v8::String::NewFromUtf8(isolate, code.c_str(), v8::NewStringType::kNormal)
            .ToLocalChecked();
    v8::TryCatch try_catch(isolate);
    v8::Local<v8::Script> script;
    if (!v8::Script::Compile(context, src).ToLocal(&script)) {
        if (try_catch.HasCaught()) {
            v8::String::Utf8Value err(isolate, try_catch.Exception());
            throw std::runtime_error(std::string("JS compile error: ") + *err);
        }
        throw std::runtime_error("JS compile error (unknown)");
    }
    v8::MaybeLocal<v8::Value> run_result = script->Run(context);
    (void)run_result;
    if (try_catch.HasCaught()) {
        v8::String::Utf8Value err(isolate, try_catch.Exception());
        throw std::runtime_error(std::string("JS run error: ") + *err);
    }
}

bool compile_and_load_cpp(Runtime& runtime, const std::string& cpp_body, const Options& opt,
                          void** out_handle) {
    *out_handle = nullptr;
    if (cpp_body.empty() || opt.no_cpp_compile) {
        if (opt.no_cpp_compile && !cpp_body.empty()) {
            std::cerr << kYellow << "[mlxf] " << kReset
                      << "Skipping /cpp ( --no-cpp-compile )" << std::endl;
        }
        return true;
    }
    log_section("cpp", kGreen);
    if (opt.verbose) {
        std::cerr << kBlue << "--- C++ (/cpp) ---" << kReset << "\n"
                  << cpp_body << std::endl;
    }

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path tmp_dir = fs::temp_directory_path();
    fs::path cpp_file = tmp_dir / ("mlxf_cpp_" + std::to_string(stamp) + "_" + std::to_string(::getpid()) + ".cpp");
    fs::path so_file = cpp_file;
    so_file.replace_extension(".so");

    {
        std::ofstream out(cpp_file);
        if (!out) {
            throw std::runtime_error("Cannot write temp C++ file");
        }
        out << "#include \"Runtime.hpp\"\n\n";
        out << cpp_body;
    }

    std::ostringstream cmd;
    cmd << "g++ -shared -fPIC -std=c++20 -O2 "
        << "-I\"" << MLXF_RUNTIME_INCLUDE_DIR << "\" "
        << "-I\"" << NLOHMANN_JSON_INCLUDE_DIR << "\" "
        << "-o \"" << so_file.string() << "\" "
        << "\"" << cpp_file.string() << "\" "
        << "2>&1";

    if (opt.verbose) {
        std::cerr << kBlue << "[mlxf] compile: " << kReset << cmd.str() << std::endl;
    }
    int rc = std::system(cmd.str().c_str());
    if (rc != 0) {
        std::cerr << kRed << "[mlxf] " << kReset << "g++ failed with code " << rc << std::endl;
        if (opt.cleanup_temps) {
            std::error_code ec;
            fs::remove(cpp_file, ec);
        }
        return false;
    }

    void* handle = dlopen(so_file.string().c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        std::cerr << kRed << "[mlxf] " << kReset << "dlopen: " << dlerror() << std::endl;
        if (opt.cleanup_temps) {
            std::error_code ec;
            fs::remove(cpp_file, ec);
            fs::remove(so_file, ec);
        }
        return false;
    }
    using InitFn = void (*)(void*);
    auto* init = reinterpret_cast<InitFn>(dlsym(handle, "init_runtime"));
    if (!init) {
        std::cerr << kRed << "[mlxf] " << kReset << "dlsym init_runtime: " << dlerror()
                  << std::endl;
        dlclose(handle);
        if (opt.cleanup_temps) {
            std::error_code ec;
            fs::remove(cpp_file, ec);
            fs::remove(so_file, ec);
        }
        return false;
    }
    init(&runtime);
    *out_handle = handle;

    if (opt.cleanup_temps) {
        std::error_code ec;
        fs::remove(cpp_file, ec);
        fs::remove(so_file, ec);
    }
    return true;
}

void install_python_runtime_proxies(Runtime& runtime, bool verbose) {
    py::dict global = py::module_::import("__main__").attr("__dict__");
    global["runtime"] = py::cast(&runtime, py::return_value_policy::reference);
    py::list names;
    for (const auto& n : runtime.list_functions()) {
        names.append(py::str(n));
    }
    global["__mlxf_names"] = names;
    py::exec(R"PY(
def __mlxf_make_proxy(__name):
    def _f(*args, **kwargs):
        if kwargs:
            return runtime.call(__name, kwargs)
        if len(args) == 1 and isinstance(args[0], dict):
            return runtime.call(__name, args[0])
        return runtime.call(__name, list(args))
    return _f

for __mlxf_n in __mlxf_names:
    existing = globals().get(__mlxf_n, None)
    if callable(existing):
        continue
    globals()[__mlxf_n] = __mlxf_make_proxy(__mlxf_n)
)PY",
             global, global);
    if (verbose) {
        std::cerr << kBlue << "[mlxf] " << kReset
                  << "Installed Python direct-call proxies for registered functions." << std::endl;
    }
}

void run_main_section(Runtime& runtime, const std::string& code, bool verbose) {
    if (code.empty()) {
        throw std::runtime_error("Missing /main section");
    }
    log_section("main", kGreen);
    if (verbose) {
        std::cerr << kBlue << "--- Main (/main) ---" << kReset << "\n"
                  << code << std::endl;
    }
    py::dict global = py::module_::import("__main__").attr("__dict__");
    global["runtime"] = py::cast(&runtime, py::return_value_policy::reference);
    std::istringstream iss(code);
    std::string line;
    std::vector<std::string> non_empty_lines;
    while (std::getline(iss, line)) {
        std::string t = trim(line);
        if (!t.empty()) {
            non_empty_lines.push_back(t);
        }
    }
    bool single_expr = non_empty_lines.size() == 1;
    if (single_expr) {
        try {
            py::object result = py::eval(non_empty_lines.front(), global, global);
            if (!result.is_none()) {
                py::print(result);
            }
            return;
        } catch (const py::error_already_set&) {
            // Fallback to regular exec when it's not a valid expression.
        }
    }
    py::exec(code, global, global);
}

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--verbose" || a == "-v") {
            o.verbose = true;
        } else if (a == "--no-cpp-compile") {
            o.no_cpp_compile = true;
        } else if (a == "--no-cleanup-temps") {
            o.cleanup_temps = false;
        } else if (a == "--help" || a == "-h") {
            std::cout << "Usage: mlxf [options] <file.poly>\n"
                      << "  --verbose, -v\n"
                      << "  --no-cpp-compile\n"
                      << "  --no-cleanup-temps\n";
            std::exit(0);
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "Unknown option: " << a << std::endl;
            std::exit(1);
        } else {
            o.poly_path = a;
        }
    }
    return o;
}

} // namespace

int main(int argc, char** argv) {
#if defined(V8_ENABLE_SANDBOX)
    // Must run before any thread that could touch V8 sandbox memory (including Python).
    v8::SandboxHardwareSupport::InitializeBeforeThreadCreation();
#endif
    Options opt = parse_args(argc, argv);
    if (opt.poly_path.empty()) {
        std::cerr << "Usage: mlxf <file.poly>\n";
        return 1;
    }

    std::unordered_map<std::string, std::string> sections;
    try {
        sections = parse_poly_file(opt.poly_path);
    } catch (const std::exception& ex) {
        std::cerr << kRed << "[mlxf] " << kReset << ex.what() << std::endl;
        return 1;
    }

    Runtime runtime;
    void* cpp_handle = nullptr;
    std::unique_ptr<V8Host> v8_host;

    {
        py::scoped_interpreter guard{};
        try {
            register_runtime_pybind_once();
            sections = autobind_sections_with_treesitter(sections, opt.verbose);

            run_python_section(runtime, sections.count("py") ? sections["py"] : "", opt.verbose);

            run_js_section(runtime, sections.count("js") ? sections["js"] : "", opt.verbose, v8_host,
                           argc > 0 && argv[0] ? argv[0] : "");

            if (!compile_and_load_cpp(runtime, sections.count("cpp") ? sections["cpp"] : "", opt,
                                      &cpp_handle)) {
                runtime.clear_functions();
                v8_host.reset();
                if (cpp_handle) {
                    dlclose(cpp_handle);
                }
                return 1;
            }

            install_python_runtime_proxies(runtime, opt.verbose);
            run_main_section(runtime, sections.count("main") ? sections["main"] : "", opt.verbose);
        } catch (const py::error_already_set& e) {
            std::cerr << kRed << "[mlxf] " << kReset << "Python error:\n"
                      << e.what() << std::endl;
            runtime.clear_functions();
            v8_host.reset();
            if (cpp_handle) {
                dlclose(cpp_handle);
            }
            return 1;
        } catch (const std::exception& ex) {
            std::cerr << kRed << "[mlxf] " << kReset << ex.what() << std::endl;
            runtime.clear_functions();
            v8_host.reset();
            if (cpp_handle) {
                dlclose(cpp_handle);
            }
            return 1;
        }
    } // finalize embedded Python before tearing down V8 (avoids use-after-free in std::functions)

    // Release JS lambdas that own v8::Global while the isolate still exists.
    runtime.clear_functions();
    v8_host.reset();
    if (cpp_handle) {
        dlclose(cpp_handle);
    }
    return 0;
}
