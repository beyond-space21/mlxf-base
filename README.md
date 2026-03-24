# mlxf – Polyglot Runtime (C++ / Python / V8 JavaScript in one file)

Single-executable runtime that reads **one `.poly` file** with sections for C++, Python, JavaScript (V8), and a `/main` entry. Cross-language calls go through a central C++ `Runtime` using JSON for arguments and return values.

## Security

**Do not run untrusted `.poly` files.** They can execute arbitrary Python and JavaScript, invoke the embedded V8 engine, and trigger **dynamic compilation and loading of C++** (`g++` + `dlopen`). Treat `.poly` sources like full code execution with compiler access.

## Dependencies

- **C++20** compiler (`g++` recommended; also used to compile `/cpp` sections at runtime)
- **CMake** 3.20+
- **Python 3** development headers and libraries
- **nlohmann/json**, **pybind11** — fetched automatically via **CMake FetchContent** (optional copies may live under `third_party/` if you prefer submodules)
- **V8** — linked from your GN build (see below). The embedder uses the **public V8 API** directly for the `/js` `runtime` object (no v8pp).
- **Google V8** — must be built separately; mlxf links against the **same layout** as the official `samples/hello-world.cc` build (see below).

## Integrating your V8 build

Point CMake at the **V8 checkout** (the directory that contains `include/` and your GN output), e.g. `~/mlxf/v8/v8`, and at the **GN `obj` directory** that contains the static libraries:

| CMake variable | Typical value (matches `g++ ... -Lout.gn/.../obj/`) |
|----------------|---------------------------------------------------|
| `V8_ROOT` | `~/mlxf/v8/v8` — contains `include/v8.h`, `include/libplatform/...` |
| `V8_BUILD_DIR` | `~/mlxf/v8/v8/out.gn/x64.release.sample/obj` — contains `libv8_monolith.a`, `libv8_libbase.a`, `libv8_libplatform.a` |

mlxf links: **`v8_monolith`**, **`v8_libbase`**, **`v8_libplatform`** (same order as a working `hello_world` link line).

**Preprocessor flags** must match your V8 GN build. Defaults follow a typical sample build:

- `MLXF_V8_POINTER_COMPRESSION=ON` → defines `V8_COMPRESS_POINTERS`
- `MLXF_V8_SANDBOX=ON` → defines `V8_ENABLE_SANDBOX`

If your GN args differ (e.g. no sandbox), turn the mismatched option **OFF** in CMake so mlxf matches the `.a` you built.

Optional: `-DMLXF_USE_LLD=ON` to link with **`lld`** (`-fuse-ld=lld`), like the V8 sample command.

At runtime, V8 startup follows **`d8`** (not the older `hello-world.cc` order): **`NewDefaultPlatform` → `InitializePlatform` → `Initialize`**, then **`InitializeICUDefaultLocation`** and **`InitializeExternalStartupData`**, then **`ArrayBuffer::Allocator::NewDefaultAllocator`** and **`Isolate::New`**. With **`V8_ENABLE_SANDBOX`**, call **`v8::SandboxHardwareSupport::InitializeBeforeThreadCreation()`** at process start (before any `mlxf` threads, including embedded Python), and never allocate `ArrayBuffer::Allocator` before **`V8::Initialize()`**.

Place **`icudtl.dat`** and snapshot blobs **next to the `mlxf` binary** or where **`InitializeExternalStartupData`** expects (same as `hello_world`).

## Build mlxf

```bash
cd PolyRun
cmake -S . -B build \
  -DV8_ROOT=$HOME/mlxf/v8/v8 \
  -DV8_BUILD_DIR=$HOME/mlxf/v8/v8/out.gn/x64.release.sample/obj
cmake --build build
```

The binary is `build/mlxf`.

You can override individual libraries if needed: `V8_MONOLITH_LIB`, `V8_LIBBASE_LIB`, `V8_LIBPLATFORM_LIB` (full paths to each `.a`).

## Usage

```bash
./build/mlxf example.poly
./build/mlxf --verbose example.poly
./build/mlxf --no-cpp-compile example.poly   # skip /cpp compile+load
./build/mlxf --no-cleanup-temps example.poly # keep temp .cpp/.so for debugging
```

## File format

Section headers use a leading slash (not `#`):

- `/cpp` — C++; must define `extern "C" void init_runtime(void* rt_ptr);` and register functions on the `Runtime*`
- `/py` — Python; may use `runtime.register(name, func)` and `runtime.call(name, args)`
- `/js` — JavaScript (V8); may use `runtime.register(name, fn)` and `runtime.call(name, obj)`
- `/main` — Python entry; always runs last and typically calls into other languages via `runtime.call`

## Execution order

1. Parse `.poly` into sections  
2. Create `Runtime`  
3. Start embedded Python and expose `runtime`  
4. Run `/py`  
5. Initialize V8, expose `runtime` on the global object (raw V8 `FunctionTemplate`), run `/js`  
6. If `/cpp` is present: write temp `.cpp`, run `g++ -shared -fPIC`, `dlopen`, call `init_runtime(&runtime)`  
7. Run `/main` as Python  

## Stretch CLI flags

`--verbose`, `--no-cpp-compile`, and `--no-cleanup-temps` are implemented as described in the project specification.
