# `.poly` File Syntax, Flow, and Rules

This document defines how `mlxf` reads and executes `.poly` files.

## Overview

A `.poly` file is a plain text file split into named sections. Each section starts with a header line:

- `/cpp`
- `/py`
- `/js`
- `/main`

Everything after a header belongs to that section until the next valid section header appears.

`mlxf` parses all sections first, runs Tree-sitter auto-bind generation, then executes in a fixed runtime order.

## Minimal Example

```text
/py
def py_echo(msg):
    return {"echo": msg}

/main
print(runtime.call("py_echo", {"msg": "hi"})["echo"])
```

## Section Semantics

### `/cpp`

- Contains C++ source code.
- At runtime, `mlxf` writes this code to a temp `.cpp`, compiles it as a shared library, loads it with `dlopen`, and calls `init_runtime(void*)`.
- If you do not define `init_runtime`, `mlxf` auto-generates one using Tree-sitter metadata.
- Auto-bind target (C++): free functions returning `json` / `nlohmann::json` and taking either:
  - no arguments, or
  - one JSON argument (`const json&` / `const nlohmann::json&`).

Manual pattern (still supported):

```cpp
extern "C" void init_runtime(void* rt_ptr) {
    auto* rt = static_cast<Runtime*>(rt_ptr);
    rt->register_function("name", [](const json& args) -> json {
        return {{"result", 42}};
    });
}
```

### `/py`

- Contains Python code executed before `/main`.
- A `runtime` object is injected into globals.
- Top-level `def` functions are auto-registered using Tree-sitter.
- Call path from `runtime.call("fn", {...})`:
  - one param named `args` / `payload` / `data` -> receives whole dict
  - zero params -> called with no args
  - otherwise -> dict unpacked as kwargs (`fn(**args_dict)`)

### `/js`

- Contains JavaScript code executed in embedded V8 before `/main`.
- A global `runtime` object is injected.
- `function name(...) { ... }` declarations are auto-registered using Tree-sitter.
- Call path from `runtime.call("fn", {...})`:
  - one param named `args` / `payload` / `data` -> receives whole object
  - zero params -> called with no args
  - otherwise -> positional extraction by parameter names from the args object

### `/main`

- Contains Python entry code.
- Executed last.
- Must exist and be non-empty, otherwise execution fails with `Missing /main section`.
- If `/main` contains a single expression line (for example `kl(1,3)`), `mlxf` evaluates it and prints the returned value.

## Runtime Execution Flow

Execution order is fixed, independent of section order in the file:

1. Parse file into sections.
2. Start embedded Python and run Tree-sitter metadata extraction.
3. Auto-generate bind wrappers/registration code for `/py`, `/js`, and `/cpp` (when needed).
4. Execute `/py` (if present).
5. Initialize V8, expose JS `runtime`, execute `/js` (if present).
6. Compile/load `/cpp` and call `init_runtime` (manual or generated, unless disabled by CLI).
7. Execute `/main` (required).

This allows `/main` to call functions registered from all languages.

## Syntax Rules (Parser Behavior)

The parser in `src/main.cpp` has exact behavior worth knowing:

- Header matching is line-based and only recognizes: `/cpp`, `/py`, `/js`, `/main`.
- Leading/trailing spaces/tabs around header lines are ignored.
  - Example: `   /py   ` is valid.
- Header matching is exact after trimming:
  - `/py` is valid
  - `/py extra` is **not** a header
  - `/python` is **not** a header
- Unknown slash lines are treated as section content, not as headers.
- Content outside any recognized section is ignored.
- Duplicate sections are allowed; the **last occurrence wins**.
  - Earlier content for the same section is overwritten by later section blocks.

## Cross-Language Contract

Interop uses JSON-shaped data:

- Args passed through `runtime.call(...)` are converted to JSON.
- Return values from registered functions should be JSON-serializable objects/values.
- In Python and JS, non-serializable return values may fail during conversion.

Recommended convention:

- Input: object/dict with named keys.
- Output: object/dict (for example `{"result": ...}`).

## Auto-Bind Rules (Tree-sitter)

Auto-bind is powered by Python Tree-sitter packages.

- Required core package: `tree_sitter`.
- Language package options:
  - single bundle: `tree_sitter_languages`
  - split packages: `tree_sitter_python`, `tree_sitter_javascript`, `tree_sitter_cpp`
- If required parser packages are missing, `mlxf` fails early with installation guidance.
- Python auto-bind discovers top-level `def` declarations.
- JavaScript auto-bind discovers function declarations (`function foo(...)`).
- C++ auto-bind discovers compatible free functions when `init_runtime` is absent.
- Manual `runtime.register(...)` and manual `init_runtime` still work and can coexist.
- After all sections are loaded, `mlxf` installs Python direct-call proxies for registered runtime functions, so `/main` and Python code can call cross-language functions as normal Python calls (`add(1, 2)`, `kl(1, 3)`).

## Important Limitation

- Direct cross-language calls are automatic from Python (`/py`, `/main`) via installed proxies.
- Raw C++ source cannot directly call a JS/Python symbol by bare name unless that C++ symbol exists/declared in C++.
  - Example: `int add(int a,int b){ return op(a,b); }` requires a C++ symbol `op` to exist.
  - Portable pattern: do cross-language orchestration from Python (`/py` or `/main`) and call C++/JS functions through normal Python direct calls (which proxy through runtime).

## Error and Failure Rules

- Missing file: `Cannot open .poly file: <path>`.
- Missing `/main`: hard failure.
- JS compile/runtime errors: hard failure with `JS compile error` / `JS run error`.
- C++ compile/load errors (`g++`, `dlopen`, `dlsym init_runtime`): hard failure.
- Python exceptions in `/py` or `/main`: hard failure.

## CLI Flags That Affect `.poly` Behavior

- `--no-cpp-compile`: parses `/cpp` but skips compile/load.
- `--no-cleanup-temps`: keeps generated temp `.cpp`/`.so` files.
- `--verbose`: prints loaded section information and section bodies.

## Authoring Guidelines

- Always include `/main`.
- Keep section names exact and standalone on their own line.
- Register functions with unique names to avoid accidental overwrite.
- Return JSON-serializable values from Python and JS handlers.
- Treat `.poly` as trusted code only (it can execute arbitrary Python/JS/C++).

## Complete Example

See `example.poly` for a working multi-language file using all four sections.
