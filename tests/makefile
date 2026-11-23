# Integration tests for the bgfx uniform upload path.
#
# Builds a headless test binary (noop renderer only - no graphics device,
# no windowing frameworks required) directly from the bgfx/bx/bimg sources
# and runs it.
#
# Usage:
#   make -C tests            # build (debug) and run the tests
#   make -C tests build      # build only
#   make -C tests run        # run only
#   make -C tests clean
#
#   CONFIG=release make -C tests   # release build (assert-rejection tests
#                                  # compile out, they require BX_CONFIG_DEBUG=1)
#
# bx and bimg are expected as siblings of this repository (the standard
# bgfx checkout layout); override BX_DIR/BIMG_DIR if they live elsewhere.

BGFX_DIR ?= ..
BX_DIR   ?= $(BGFX_DIR)/../bx
BIMG_DIR ?= $(BGFX_DIR)/../bimg

CONFIG   ?= debug

CXX      ?= clang++

OUT_DIR  := .build/$(CONFIG)
TEST_BIN := $(OUT_DIR)/uniform-upload-test

# --- Configuration ------------------------------------------------------------

ifeq ($(CONFIG),debug)
CONFIG_DEFINES := BX_CONFIG_DEBUG=1
CONFIG_FLAGS   := -g -O0
else
CONFIG_DEFINES := BX_CONFIG_DEBUG=0 NDEBUG
CONFIG_FLAGS   := -O2
endif

# Noop renderer only: the upload path under test (encoder, uniform buffer
# stream, render-side uniform application) is identical for all backends,
# and this keeps the tests free of any graphics device or windowing
# framework dependency.
RENDERER_DEFINES := \
	BGFX_CONFIG_RENDERER_AGC=0 \
	BGFX_CONFIG_RENDERER_DIRECT3D11=0 \
	BGFX_CONFIG_RENDERER_DIRECT3D12=0 \
	BGFX_CONFIG_RENDERER_GNM=0 \
	BGFX_CONFIG_RENDERER_METAL=0 \
	BGFX_CONFIG_RENDERER_NVN=0 \
	BGFX_CONFIG_RENDERER_OPENGL=0 \
	BGFX_CONFIG_RENDERER_OPENGLES=0 \
	BGFX_CONFIG_RENDERER_VULKAN=0 \
	BGFX_CONFIG_RENDERER_WEBGPU=0

DEFINES := $(CONFIG_DEFINES) $(RENDERER_DEFINES) BIMG_CONFIG_DECODE_ENABLE=0

INCLUDES := \
	-I$(BGFX_DIR)/include \
	-I$(BGFX_DIR)/src \
	-I$(BGFX_DIR)/3rdparty \
	-I$(BGFX_DIR)/3rdparty/khronos \
	-I$(BX_DIR)/include \
	-I$(BX_DIR)/3rdparty \
	-I$(BIMG_DIR)/include

UNAME := $(shell uname)
ifeq ($(UNAME),Darwin)
INCLUDES += -I$(BX_DIR)/include/compat/osx
# bx/bgfx reference CoreFoundation/Foundation and the ObjC runtime even
# with all real renderers compiled out.
LDLIBS   := -framework CoreFoundation -framework Foundation -lobjc
else
INCLUDES += -I$(BX_DIR)/include/compat/linux
LDLIBS   := -ldl -lrt
endif

CXXFLAGS := -std=gnu++20 -Wall -Wextra -Wno-unused-parameter -pthread $(CONFIG_FLAGS) \
	$(addprefix -D,$(DEFINES)) $(INCLUDES)

# --- Sources ------------------------------------------------------------------

TEST_SRCS := \
	src/test_main.cpp \
	src/test_uniform_api.cpp \
	src/test_uniform_layout.cpp \
	$(BX_DIR)/3rdparty/catch/catch_amalgamated.cpp

# bgfx core + noop renderer (amalgamated TU; all real backends are
# compiled out via the renderer defines above).
BGFX_SRCS := $(BGFX_DIR)/src/amalgamated.cpp
BX_SRCS   := $(BX_DIR)/src/amalgamated.cpp
BIMG_SRCS := $(BIMG_DIR)/src/image.cpp $(BIMG_DIR)/src/image_encode_wic.cpp

TEST_OBJS := \
	$(OUT_DIR)/test_main.o \
	$(OUT_DIR)/test_uniform_api.o \
	$(OUT_DIR)/test_uniform_layout.o \
	$(OUT_DIR)/catch_amalgamated.o
LIB_OBJS  := $(OUT_DIR)/bgfx_amalgamated.o $(OUT_DIR)/bx_amalgamated.o $(OUT_DIR)/bimg_image.o $(OUT_DIR)/bimg_image_encode_wic.o

# --- Rules --------------------------------------------------------------------

.PHONY: all build run clean

all: run

build: $(TEST_BIN)

run: $(TEST_BIN)
	$(TEST_BIN)

$(TEST_BIN): $(TEST_OBJS) $(LIB_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)
ifeq ($(UNAME),Darwin)
	# Unsigned x86_64 binaries can hang at exec while Gatekeeper assesses
	# them; an ad-hoc signature avoids that.
	codesign --sign - --force $@
endif

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

# Test sources.
$(OUT_DIR)/test_main.o: src/test_main.cpp src/test.h | $(OUT_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OUT_DIR)/test_uniform_api.o: src/test_uniform_api.cpp src/test.h | $(OUT_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OUT_DIR)/test_uniform_layout.o: src/test_uniform_layout.cpp src/test.h | $(OUT_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OUT_DIR)/catch_amalgamated.o: $(BX_DIR)/3rdparty/catch/catch_amalgamated.cpp | $(OUT_DIR)
	$(CXX) $(CXXFLAGS) -DCATCH_AMALGAMATED_CUSTOM_MAIN -w -c -o $@ $<

# Library sources (warnings suppressed, they are not what is being tested).
$(OUT_DIR)/bgfx_amalgamated.o: $(BGFX_SRCS) | $(OUT_DIR)
	$(CXX) $(CXXFLAGS) -w -c -o $@ $<

$(OUT_DIR)/bx_amalgamated.o: $(BX_SRCS) | $(OUT_DIR)
	$(CXX) $(CXXFLAGS) -w -c -o $@ $<

$(OUT_DIR)/bimg_image.o: $(BIMG_DIR)/src/image.cpp | $(OUT_DIR)
	$(CXX) $(CXXFLAGS) -w -c -o $@ $<

$(OUT_DIR)/bimg_image_encode_wic.o: $(BIMG_DIR)/src/image_encode_wic.cpp | $(OUT_DIR)
	$(CXX) $(CXXFLAGS) -w -c -o $@ $<

clean:
	rm -rf .build
