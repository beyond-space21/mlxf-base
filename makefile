# Example: configure and build mlxf against a local V8 tree (same layout as g++ in v8/v8 with out.gn/x64.release.sample/obj).
V8_HOME ?= $(HOME)/mlxf/v8/v8
BUILD_DIR ?= $(HOME)/mlxf/PolyRun/build

configure:
	cmake -S $(HOME)/mlxf/PolyRun -B $(BUILD_DIR) \
	  -DV8_ROOT=$(V8_HOME) \
	  -DV8_BUILD_DIR=$(V8_HOME)/out.gn/x64.release.sample/obj

buildit:
	cmake --build $(BUILD_DIR) --target mlxf -j

# Optional: same linker as V8 hello-world sample
configure-lld:
	cmake -S $(HOME)/mlxf/PolyRun -B $(BUILD_DIR) \
	  -DV8_ROOT=$(V8_HOME) \
	  -DV8_BUILD_DIR=$(V8_HOME)/out.gn/x64.release.sample/obj \
	  -DMLXF_USE_LLD=ON

.PHONY: configure buildit configure-lld
