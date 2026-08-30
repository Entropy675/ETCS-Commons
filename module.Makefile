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
LOCAL_HEADERS := $(wildcard $(PLATFORM_DIR)/*.h) $(wildcard $(TARGET_BASE_NAME)/*.h)
MODULE_HEADERS := $(sort $(LOCAL_HEADERS) $(TARGET_BASE_NAME).h)

# List all necessary dependencies
MODULE_DEPS := $(SRC_MODULE) $(MODULE_HEADERS) $(GLOBAL_HEADERS)

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
	$(CXX) $(CXXFLAGS) $(EXTRADEFINES) $(LDFLAGS) -o $@ $(SRC_MODULE)

# -------------------------
# Clean rule
.PHONY: clean
clean:
	rm -f $(FINAL_TARGET) *.o $(PLATFORM_DIR)/*.hash $(HASH_HEADER)
