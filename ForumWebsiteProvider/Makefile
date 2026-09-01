# ====================================================================
# GENERATED FILE -- do not edit.
#
#   source:      <ace tool>/manifests/default.json
#                (ForumWebsiteProvider declares nothing of its own -- no
#                 vendored dependencies, no unusual flags)
#   regenerate:  ace manifest generate ForumWebsiteProvider
#
# Edits here are lost the next time this file is regenerated, which
# happens automatically whenever it is missing. Change the manifest.
# ====================================================================

TARGET_BASE_NAME := ForumWebsiteProvider
GLOBAL_HEADERS := ../../ontology.h ../../ontology_hashes.h ../../libs.h ../../libs_hashes.h ../../core_defs.h
SRC_MODULE := $(TARGET_BASE_NAME).cc
HASH_HEADER := module_hashes.h

UNAME_S := $(shell uname -s)
ARCH    := $(shell uname -m)
NPROC   := $(shell nproc)
CXX := g++
CC  := gcc

# Debug info: make DEBUG=1. OFF by default.
#
# -g roughly doubles a module's on-disk size, and the hand-written
# Makefiles disagreed about it -- NetworkProvider and DatabaseProvider
# carried it, ChessProvider did not. Making it a mode rather than a
# baseline is what lets all four agree without picking a winner.
#
# A sanitizer build turns it on regardless: a sanitizer report without
# line numbers is a list of hex addresses, which is the situation
# ASAN/TSAN exist to get you out of.
DEBUGFLAGS :=
DBG_SUFFIX :=
ifeq ($(DEBUG),1)
    DEBUGFLAGS := -g
    DBG_SUFFIX := _dbg
endif

# ------------------------------------------------------------------
# Sanitizers: make ASAN=1 / make TSAN=1.
#
# Mutually exclusive -- they instrument the same code paths and cannot
# coexist in one binary.
#
# The module and whatever loader dlopens it MUST be built the same way.
# They share one address space, and a sanitizer runtime that sees only
# half of it reports nonsense.
#
# Vendored dependencies are NOT rebuilt for ASan: it interposes malloc
# and free process-wide, so allocation faults inside an uninstrumented
# library are still caught -- that is exactly how mbedTLS's PSA
# double-free was found. TSan is different: it only sees races in code
# it instrumented, so TSAN=1 rebuilds them.
# ------------------------------------------------------------------
SANITIZE   :=
SAN_SUFFIX :=
ifeq ($(ASAN),1)
ifeq ($(TSAN),1)
    $(error ASAN=1 and TSAN=1 are mutually exclusive)
endif
    SANITIZE   := -fsanitize=address -fno-omit-frame-pointer
    SAN_SUFFIX := _asan
endif
ifeq ($(TSAN),1)
    SANITIZE   := -fsanitize=thread -fno-omit-frame-pointer
    SAN_SUFFIX := _tsan
endif

# Vendored dependencies follow only the TSan half, per the note above:
# rebuilding them costs minutes, and for ASan it buys only overflow
# detection INSIDE the dependency -- allocation faults there are caught
# either way.
DEP_SANITIZE   :=
DEP_SAN_SUFFIX :=
ifeq ($(TSAN),1)
    DEP_SANITIZE   := -fsanitize=thread -fno-omit-frame-pointer
    DEP_SAN_SUFFIX := _tsan
endif

# Symbols are not optional under a sanitizer -- see the DEBUG note.
ifneq ($(SANITIZE),)
    DEBUGFLAGS := -g
endif

# Build stamp -- architecture, sanitizer, and ABI-define set.
#
# A REAL prerequisite of the final target, not order-only: switching
# any of the three must force a relink, and make has no other way to
# notice that CXXFLAGS changed. Mixing instrumented and uninstrumented
# objects in one .so links cleanly and then misbehaves at runtime,
# which is precisely what this stamp exists to make impossible. The
# architecture half matters for the same reason it always did: a tree
# carried to another machine would otherwise relink objects built for
# the architecture it came from.
BUILD_STAMP := .ace_build_$(ARCH)$(DBG_SUFFIX)$(SAN_SUFFIX)

CXXFLAGS := -std=c++17 \
            -fvisibility=hidden \
            -Wall \
            -fPIC \
            -Wextra \
            -O2 \
            -DETCS_MODULE_NAME=\"$(TARGET_BASE_NAME)\" \
            -I. \
            -I../.. \
            -fpermissive \
            -pipe \
            -fno-plt \
            $(DEBUGFLAGS) \
            $(SANITIZE) \
            $(CUSTOM_CXXFLAGS)

ifeq ($(UNAME_S),Linux)
    HOST_PLATFORM := Linux
    FINAL_TARGET := $(TARGET_BASE_NAME).so
    LDFLAGS := -shared -fuse-ld=gold -Wl,--threads -Wl,--thread-count,$(NPROC) -pthread -Wl,--version-script=exports.map
    SYSLIBS := -ldl
else ifeq ($(UNAME_S),Windows_NT)
    HOST_PLATFORM := Win
    FINAL_TARGET := $(TARGET_BASE_NAME).dll
    LDFLAGS := -shared -fuse-ld=gold -Wl,--threads -Wl,--thread-count,$(NPROC)
    SYSLIBS := -lws2_32
else
    $(error Unsupported platform: $(UNAME_S))
endif

# Implementation layout: auto.
#
# An OS/ directory means one implementation serves every target and
# wins outright; otherwise the host platform's own directory is used.
# A module with NO platform directory at all resolves to a name that
# matches nothing, which is correct -- its headers live beside it.
#
# Safe to infer now in a way it was not before: MODULE_HEADERS spans
# every platform directory regardless (see the hash-scope note), so
# guessing this wrong can no longer silently empty the attestation.
# It only decides what gets -I'd and what a rebuild depends on.
PLATFORM_DIR := $(HOST_PLATFORM)
ifneq ($(wildcard OS/.),)
    PLATFORM_DIR := OS
endif

CXXFLAGS += -I$(PLATFORM_DIR)

# Header discovery. Vendored trees are deliberately NOT globbed here
# unless their manifest entry sets hash_scope: every upstream commit
# would otherwise churn this module's ABI attestation for a change no
# consumer can observe.
LOCAL_HEADERS := $(wildcard $(TARGET_BASE_NAME)/*.h)
PLATFORM_LOCAL_HEADERS := $(wildcard $(PLATFORM_DIR)/*.h)

# Every platform directory this module actually carries, whichever
# one this build compiles against.
POTENTIAL_DIRS := Linux Win Web OS
EXISTING_DIRS  := $(foreach d,$(POTENTIAL_DIRS),$(if $(wildcard $(d)/.),$(d)))
ALL_PLATFORM_HEADERS := $(foreach d,$(EXISTING_DIRS),$(wildcard $(d)/*.h))

# HASH SCOPE -- every platform's headers, not just the active one.
#
# The manifest hash is an attestation about the MODULE, so it must not
# depend on the machine that produced it. Hashing only $(PLATFORM_DIR)
# means the same commit attests differently on Linux and on Windows,
# and `ace abi` reports drift that is really just an OS difference --
# indistinguishable, at the point you read it, from a genuine contract
# change. WindowProvider's hand-written Makefile got this right and it
# was lost when the four modules were unified on the per-platform
# template; this restores it for all of them.
MODULE_HEADERS := $(sort $(LOCAL_HEADERS) $(ALL_PLATFORM_HEADERS) $(TARGET_BASE_NAME).h)

# COMPILE SCOPE -- only the platform being built, since that is what a
# rebuild should actually depend on. Editing Win/ must not relink a
# Linux build; it must still change the hash above.
COMPILE_HEADERS := $(sort $(LOCAL_HEADERS) $(PLATFORM_LOCAL_HEADERS) $(TARGET_BASE_NAME).h)
MODULE_DEPS := $(SRC_MODULE) $(COMPILE_HEADERS) $(GLOBAL_HEADERS)

.PHONY: all clean
all: $(FINAL_TARGET)
	@echo "✓ Built $(FINAL_TARGET) for $(UNAME_S) ($(ARCH))"

$(BUILD_STAMP):
	@rm -f .ace_build_*
	@touch $@

$(HASH_HEADER): $(MODULE_HEADERS)
	@echo "// Generated Registration - do not edit" > $@
	@for f in $(MODULE_HEADERS); do \
	    HASH=$$(cat $$f | openssl dgst -sha256 | awk '{print $$NF}'); \
	    FULL_NAME=$$(basename $$f); \
	    VAR_NAME=$$(echo $$FULL_NAME | sed 's/\./_/g'); \
	    printf 'inline const bool _reg_%s = []() { ETCS::FlatMap<ETCS::Buffer, ETCS::Buffer>::setArena(&ETCS::MemoryArena::getInstance()); ETCS::Entity::getManifest()["%s"] = "%s"; return true; }();\n' "$$VAR_NAME" "$$FULL_NAME" "$$HASH" >> $@; \
	done

# Archives come AFTER the translation unit that needs them: gold does
# not rescan an archive it has already passed, so a dependency listed
# among the flags resolves only by accident.
$(FINAL_TARGET): $(MODULE_DEPS) $(HASH_HEADER) $(BUILD_STAMP)
	$(CXX) $(CXXFLAGS) $(EXTRADEFINES) $(LDFLAGS) -o $@ $(SRC_MODULE) $(SYSLIBS)

# Header dependencies.
#
# A separate -MM pass rather than -MMD on the build rule above: that rule
# compiles AND links in one command, so gcc writes a single <target>.d
# holding only the LAST source's deps -- a vendored C file, not the module.
#
# The order-only prerequisites are load-bearing. `-include $(DEPFILE)`
# makes make build DEPFILE before ANY goal, and the -MM pass preprocesses
# the module for real -- so a vendored header that has not been fetched
# yet is a fatal error on a clean tree rather than a missing edge. Every
# fetch and build marker is listed here for exactly that reason.
DEPFILE := $(SRC_MODULE:.cc=.d)
$(DEPFILE): $(SRC_MODULE) $(HASH_HEADER)
	@$(CXX) $(CXXFLAGS) $(EXTRADEFINES) -MM -MP -MT '$(FINAL_TARGET)' -MF $@ $(SRC_MODULE)

# Skipped for `clean`: -include makes make build DEPFILE before any goal,
# and a tree too broken to preprocess must still be cleanable.
ifeq (,$(filter clean,$(MAKECMDGOALS)))
-include $(DEPFILE)
endif

clean:
	rm -f $(FINAL_TARGET) *.o $(HASH_HEADER) $(DEPFILE) .ace_build_*
	rm -rf .ace_obj
	@echo "✓ Cleaned $(TARGET_BASE_NAME) (vendored sources preserved)"
