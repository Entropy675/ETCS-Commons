# Module level Makefile
# -------------------------
TARGET_BASE_NAME := $(notdir $(CURDIR))
# -------------------------
GLOBAL_HEADERS := ../../ontology.h ../../ontology_hashes.h ../../libs.h ../../libs_hashes.h ../../core_defs.h
SRC_MODULE := $(TARGET_BASE_NAME).cc
HASH_HEADER := module_hashes.h
# -------------------------
# --- GLFW Static Bootstrap ---
GLFW_DIR := $(CURDIR)/glfw
GLFW_LIB := $(GLFW_DIR)/build/src/libglfw3.a
GLFW_INCLUDE := $(GLFW_DIR)/include
# -------------------------
# --- Platform Detection ---
UNAME_S := $(shell uname -s)
HOST_PLATFORM := Unknown
ifeq ($(UNAME_S),Linux)
    HOST_PLATFORM := Linux
else ifeq ($(UNAME_S),Windows_NT)
    HOST_PLATFORM := Win
endif
# --- Priority Resolution ---
PLATFORM_DIR := $(HOST_PLATFORM)
ifneq ($(wildcard OS/.),)
    PLATFORM_DIR := OS
endif
ifdef EMSCRIPTEN
    PLATFORM_DIR := Web
endif
# -------------------------
# Compiler settings
CXX := g++
CXXFLAGS := -std=c++17 -fvisibility=hidden -g -Wall -fPIC -Wextra -O2 -DETCS_MODULE_NAME=\"$(TARGET_BASE_NAME)\" -I../.. -I$(GLFW_INCLUDE) -pipe -fno-plt
# -------------------------
# --- Build Configuration ---

# 1. First, set up the include directory based on your Priority Resolution
CXXFLAGS += -I$(PLATFORM_DIR)

# 2. Next, set up the Linker Flags based on the ACTUAL target environment
ifdef EMSCRIPTEN
    CXX := em++
    CXXFLAGS += -s SIDE_MODULE=1 -s USE_GLFW=3
    FINAL_TARGET := $(TARGET_BASE_NAME).so
else ifeq ($(HOST_PLATFORM),Win)
    LDFLAGS := -shared -fuse-ld=gold \
               $(GLFW_LIB) -lgdi32 -luser32 -lshell32
    FINAL_TARGET := $(TARGET_BASE_NAME).dll
else ifeq ($(HOST_PLATFORM),Linux)
    # Now this will actually trigger!
    WHOLE_ARCHIVE := -Wl,--whole-archive $(GLFW_LIB) -Wl,--no-whole-archive
    CXXFLAGS += -fvisibility=hidden -fvisibility-inlines-hidden
    
    # Notice I added $(WHOLE_ARCHIVE) into the LDFLAGS here, 
    # as it was missing from your Linux LDFLAGS assignment!
    LDFLAGS := -shared -fuse-ld=gold \
               -Wl,-Bsymbolic \
               $(WHOLE_ARCHIVE) \
               -Wl,--version-script=$(CURDIR)/exports.map \
               -lX11 -lpthread -luring -lXrandr -lXi -ldl -lwayland-client -lxkbcommon
    FINAL_TARGET := $(TARGET_BASE_NAME).so
else
    # Fallback
    LDFLAGS := -shared -pthread -luring -Wl,-Bsymbolic -fuse-ld=gold \
               $(GLFW_LIB) -lX11 -lpthread -lXrandr -lXi -ldl
    FINAL_TARGET := $(TARGET_BASE_NAME).so
endif
# -------------------------
# --- Header Discovery ---
LOCAL_HEADERS := $(wildcard *.h)
PLATFORM_LOCAL_HEADERS := $(wildcard $(PLATFORM_DIR)/*.h)
POTENTIAL_DIRS := Linux Win Web OS
EXISTING_DIRS := $(foreach dir,$(POTENTIAL_DIRS),$(if $(wildcard $(dir)/.),$(dir)))
ALL_PLATFORM_HEADERS := $(foreach dir,$(EXISTING_DIRS),$(wildcard $(dir)/*.h))
MODULE_HEADERS := $(sort $(LOCAL_HEADERS) $(ALL_PLATFORM_HEADERS))
COMPILE_HEADERS := $(sort $(LOCAL_HEADERS) $(PLATFORM_LOCAL_HEADERS))
MODULE_DEPS := $(SRC_MODULE) $(COMPILE_HEADERS) $(GLOBAL_HEADERS)
# -------------------------
.PHONY: all
all: $(FINAL_TARGET)
	@echo "✓ Built $(FINAL_TARGET) for $(PLATFORM_DIR)"

# --- GLFW Bootstrap ---
$(GLFW_LIB):
	@echo "→ GLFW static lib not found, bootstrapping..."
	@if [ ! -d "$(GLFW_DIR)/.git" ]; then \
		git clone https://github.com/glfw/glfw.git $(GLFW_DIR); \
	fi
	cmake -S $(GLFW_DIR) -B $(GLFW_DIR)/build \
		-DBUILD_SHARED_LIBS=OFF \
		-DGLFW_BUILD_EXAMPLES=OFF \
		-DGLFW_BUILD_TESTS=OFF \
		-DGLFW_BUILD_DOCS=OFF
	cmake --build $(GLFW_DIR)/build
	@echo "✓ GLFW static lib ready"

$(HASH_HEADER): $(MODULE_HEADERS)
	@echo "// Generated Registration - do not edit" > $@
	@for f in $(MODULE_HEADERS); do \
		HASH=$$(cat $$f | openssl dgst -sha256 | awk '{print $$NF}'); \
		FULL_NAME=$$(basename $$f); \
		VAR_NAME=$$(echo $$FULL_NAME | sed 's/\./_/g'); \
		echo "inline const bool _reg_$$VAR_NAME = []() { \
		    ETCS::FlatMap<ETCS::Buffer, ETCS::Buffer>::setArena(&ETCS::MemoryArena::getInstance());  \
			ETCS::Entity::getManifest()[\"$$FULL_NAME\"] = \"$$HASH\"; \
			return true; \
		}();" >> $@; \
	done

$(FINAL_TARGET): $(GLFW_LIB) $(MODULE_DEPS) $(HASH_HEADER)
	$(CXX) $(CXXFLAGS) $(EXTRADEFINES) $(SRC_MODULE) $(LDFLAGS) -o $@
# -------------------------
# Header dependencies
#
# A separate -MM pass rather than -MMD on the build rule above: that rule
# compiles AND links in one command, so gcc writes a single <target>.d holding
# only the LAST source's deps -- a vendored C file, not the module. -MF does not
# help; every source in the command writes that same name.
#
# -MT names FINAL_TARGET directly, so the generated line feeds straight into the
# rule above. -MP adds phony header targets so a deleted header is not a hard
# error.
#
# This is what makes a core/ edit rebuild this module at all: MODULE_DEPS lists
# a handful of global headers by hand and none of core/, which is how a module
# and the loader came to be built from different GapSlot layouts out of one
# tree.
#
# HASH_HEADER is a prerequisite because -include makes make build DEPFILE
# BEFORE anything else, and the -MM pass preprocesses SRC_MODULE for real --
# so a generated header that does not exist yet is a fatal error on a clean
# tree, not a missing dependency edge.
DEPFILE := $(SRC_MODULE:.cc=.d)
$(DEPFILE): $(SRC_MODULE) $(HASH_HEADER)
	@$(CXX) $(CXXFLAGS) $(EXTRADEFINES) -MM -MP -MT '$(FINAL_TARGET)' -MF $@ $(SRC_MODULE)
# Skipped for `clean`: -include makes make build DEPFILE before any goal, and
# a tree too broken to preprocess must still be cleanable.
ifeq (,$(filter clean,$(MAKECMDGOALS)))
-include $(DEPFILE)
endif

# -------------------------
.PHONY: clean
clean:
	rm -f $(FINAL_TARGET) *.o $(HASH_HEADER) $(foreach dir,$(EXISTING_DIRS),$(dir)/*.hash) $(DEPFILE)
	@if [ -d "$(GLFW_DIR)/build" ]; then \
		echo "→ Removing GLFW build artifacts..."; \
		rm -rf $(GLFW_DIR)/build; \
		echo "✓ GLFW build cleaned (source preserved)"; \
	fi
