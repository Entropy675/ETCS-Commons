# Module level Makefile - ACE Ecosystem

# CONFIG:
# CUSTOM_CXXFLAGS := 

# -------------------------
# Target/Module Base Name: Dynamically set to the folder name
TARGET_BASE_NAME := $(notdir $(CURDIR))

# -------------------------
# Global Headers: Utilizing the ETCS symlink for stability
# We point these to the ETCS handle which ace setup created.
GLOBAL_HEADERS := ../../ontology.h ../../ontology_hashes.h ../../libs.h ../../libs_hashes.h ../../core_defs.h

# -------------------------
# Source file 
SRC_MODULE := $(TARGET_BASE_NAME).cc
HASH_HEADER := module_hashes.h

# -------------------------
# CChess engine sources, compiled INTO this module so ChessProvider.so is
# self-contained -- no libcchess.so at runtime, nothing to install beside it.
# Without this the module links with Board's methods left undefined and dlopen
# fails at load ("undefined symbol: _ZN5Board11isCheckmateEv"), because
# Board.h only DECLARES them.
#
# The exclusions are CChess's own loader and concrete renderers -- exactly the
# layer ETCS replaces. main.cc is a standalone entry point and ChessGame*.cc is
# the old loader; NcView/WinView are the ncurses/Windows renderers the runtime
# now owns (and would drag in ncurses as a link dependency).
#
# View.cc is deliberately NOT excluded, despite being part of that same layer:
# Log.cc owns a std::vector<View*> registry and delViewById calls
# View::getId(), so the abstract base is a dependency of the LOGGING layer, not
# just of rendering. It comes along until that registry moves out of Log.
CCHESS_DIR := $(TARGET_BASE_NAME)/CChess/src
CCHESS_EXCLUDE := $(CCHESS_DIR)/main.cc \
                  $(CCHESS_DIR)/ChessGame.cc \
                  $(CCHESS_DIR)/ChessGameLinux.cc \
                  $(CCHESS_DIR)/ChessGameWindows.cc \
                  $(CCHESS_DIR)/NcView.cc \
                  $(CCHESS_DIR)/WinView.cc
CCHESS_SRC := $(filter-out $(CCHESS_EXCLUDE), $(wildcard $(CCHESS_DIR)/*.cc)) \
              $(wildcard $(CCHESS_DIR)/piece_behav/*.cc)

# -------------------------
# Detect platform
UNAME_S := $(shell uname -s)

# -------------------------
# Compiler settings
CXX := g++
# We include $(CURDIR) and $(CURDIR)/ETCS to allow for both 
CXXFLAGS := -std=c++17 -fvisibility=hidden -fpermissive -Wall -fPIC -Wextra -O2 \
            -I. -I../.. -pipe -fno-plt $(CUSTOM_CXXFLAGS)

# -------------------------
# Platform-specific paths and targets
ifeq ($(UNAME_S),Linux)
    PLATFORM_DIR := Linux
    # Added version-script to enforce the exports.map firewall
    LDFLAGS := -shared -pthread -fuse-ld=gold -Wl,--threads \
               -DETCS_MODULE_NAME=\"$(TARGET_BASE_NAME)\" \
               -Wl,--thread-count,$(shell nproc) -ldl \
               -Wl,--version-script=exports.map
    FINAL_TARGET := $(TARGET_BASE_NAME).so
else ifeq ($(UNAME_S),Windows_NT)
    PLATFORM_DIR := Win
    # Windows linkers (link.exe/lld-link) use .def files or __declspec(dllexport)
    # exports.map will not work here, so we keep the standard flags.
    LDFLAGS := -shared -lws2_32 
    FINAL_TARGET := $(TARGET_BASE_NAME).dll
else
    $(error Unsupported platform: $(UNAME_S))
endif
# -------------------------
# MODULE HEADERS: Local module headers
#
# Deliberately does NOT reach into $(CCHESS_DIR): this glob feeds the manifest
# hashes below, which should pin THIS module's own contract surface
# (ChessProvider.h, ChessProvider/ChessGame.h), not the vendored engine's
# internals -- otherwise every upstream CChess commit churns the module hash
# and invalidates the ABI attestation for a change no consumer can observe.
LOCAL_HEADERS := $(wildcard $(PLATFORM_DIR)/*.h) $(wildcard $(TARGET_BASE_NAME)/*.h)
MODULE_HEADERS := $(sort $(LOCAL_HEADERS) $(TARGET_BASE_NAME).h)

# List all necessary dependencies
MODULE_DEPS := $(SRC_MODULE) $(CCHESS_SRC) $(MODULE_HEADERS) $(GLOBAL_HEADERS)

# -------------------------
# Build Targets
.PHONY: all
all: $(FINAL_TARGET)
	@echo "✓ Built $(FINAL_TARGET) for $(UNAME_S) using ACE_ROOT: $(ACE_ROOT)"

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
    
# -------------------------
# Build rule
$(FINAL_TARGET): $(MODULE_DEPS) $(HASH_HEADER)
	$(CXX) $(CXXFLAGS) $(EXTRADEFINES) $(LDFLAGS) -o $@ $(SRC_MODULE) $(CCHESS_SRC)
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
# Clean rule
.PHONY: clean
clean:
	rm -f $(FINAL_TARGET) *.o $(PLATFORM_DIR)/*.hash $(HASH_HEADER) $(DEPFILE)
