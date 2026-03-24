cmake_minimum_required(VERSION 3.20)
project(mlxf LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Python3 COMPONENTS Interpreter Development REQUIRED)
find_package(Threads REQUIRED)

include(FetchContent)
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

FetchContent_Declare(
  pybind11
  GIT_REPOSITORY https://github.com/pybind/pybind11.git
  GIT_TAG v2.11.1
)
FetchContent_MakeAvailable(pybind11)

# V8 checkout root (contains include/ and out.gn/... — same as running g++ from this directory with -I. -Iinclude)
set(V8_ROOT "$ENV{HOME}/mlxf/v8/v8" CACHE PATH "Path to V8 source tree (e.g. .../v8/v8)")
# GN output dir with static libraries (e.g. out.gn/x64.release.sample/obj next to your hello_world link line)
set(V8_BUILD_DIR "${V8_ROOT}/out.gn/x64.release.sample/obj" CACHE PATH "Path to V8 GN output dir containing libv8_*.a")

find_library(V8_MONOLITH_LIB
  NAMES v8_monolith libv8_monolith
  PATHS "${V8_BUILD_DIR}"
  NO_DEFAULT_PATH
)

find_library(V8_LIBBASE_LIB
  NAMES v8_libbase libv8_libbase
  PATHS "${V8_BUILD_DIR}"
  NO_DEFAULT_PATH
)

find_library(V8_LIBPLATFORM_LIB
  NAMES v8_libplatform libv8_libplatform
  PATHS "${V8_BUILD_DIR}"
  NO_DEFAULT_PATH
)

set(V8_MONOLITH_LIB "${V8_MONOLITH_LIB}" CACHE FILEPATH "Path to libv8_monolith.a")
set(V8_LIBBASE_LIB "${V8_LIBBASE_LIB}" CACHE FILEPATH "Path to libv8_libbase.a")
set(V8_LIBPLATFORM_LIB "${V8_LIBPLATFORM_LIB}" CACHE FILEPATH "Path to libv8_libplatform.a")

if(NOT V8_MONOLITH_LIB)
  message(WARNING "libv8_monolith not found in V8_BUILD_DIR=${V8_BUILD_DIR}. Set V8_ROOT / V8_BUILD_DIR.")
endif()

# Match your working g++ line: -DV8_COMPRESS_POINTERS -DV8_ENABLE_SANDBOX
option(MLXF_V8_POINTER_COMPRESSION "Define V8_COMPRESS_POINTERS (must match V8 build)" ON)
option(MLXF_V8_SANDBOX "Define V8_ENABLE_SANDBOX (must match V8 build)" ON)

set(V8_LINK_LIBS "")
if(V8_MONOLITH_LIB)
  list(APPEND V8_LINK_LIBS ${V8_MONOLITH_LIB})
endif()
if(V8_LIBBASE_LIB)
  list(APPEND V8_LINK_LIBS ${V8_LIBBASE_LIB})
endif()
if(V8_LIBPLATFORM_LIB)
  list(APPEND V8_LINK_LIBS ${V8_LIBPLATFORM_LIB})
endif()

add_executable(mlxf
  src/main.cpp
  src/Runtime.cpp
)

# Same as g++ -Iinclude from the V8 checkout (v8.h, libplatform/..., etc.)
target_include_directories(mlxf PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${V8_ROOT}/include"
)

target_compile_definitions(mlxf PRIVATE
  MLXF_RUNTIME_INCLUDE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src"
  NLOHMANN_JSON_INCLUDE_DIR="${json_SOURCE_DIR}/include"
)

if(MLXF_V8_POINTER_COMPRESSION)
  target_compile_definitions(mlxf PRIVATE V8_COMPRESS_POINTERS V8_31BIT_SMIS_ON_64BIT_ARCH)
endif()
if(MLXF_V8_SANDBOX)
  target_compile_definitions(mlxf PRIVATE V8_ENABLE_SANDBOX)
endif()

target_link_libraries(mlxf PRIVATE
  nlohmann_json::nlohmann_json
  pybind11::embed
  Python3::Python
  Threads::Threads
  ${CMAKE_DL_LIBS}
  ${V8_LINK_LIBS}
)

if(UNIX AND NOT APPLE)
  target_link_options(mlxf PRIVATE "-Wl,--export-dynamic")
elseif(APPLE)
  target_link_options(mlxf PRIVATE "-Wl,-export_dynamic")
endif()

option(MLXF_USE_LLD "Link with LLVM lld (-fuse-ld=lld), matching a typical V8 sample build" OFF)
if(MLXF_USE_LLD)
  target_link_options(mlxf PRIVATE "-fuse-ld=lld")
endif()
