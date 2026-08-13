# Module level Makefile - ACE Ecosystem + SQLite Integration

# -------------------------
# Target/Module Base Name
TARGET_BASE_NAME := $(notdir $(CURDIR))

# -------------------------
# Global ACE Headers
GLOBAL_HEADERS := ../../ontology.h ../../ontology_hashes.h ../../libs.h ../../libs_hashes.h ../../core_defs.h

# -------------------------
# SQLite Specific Config
SQLITE_DIR := sqlite
SQLITE_AMALGAMATION := $(SQLITE_DIR)/sqlite3.c
SQLITE_OBJ := sqlite3.o
# Adjust SQLite features here (e.g., thread safety, memory management)
SQLITE_FLAGS := -DSQLITE_ENABLE_COLUMN_METADATA -DSQLITE_ENABLE_RTREE -DSQLITE_THREADSAFE=1

# -------------------------
# Source & Hashes
SRC_MODULE := $(TARGET_BASE_NAME).cc
HASH_HEADER := module_hashes.h

# -------------------------
# Platform Detection
UNAME_S := $(shell uname -s)
CXX := g++
CC  := gcc

# ACE Flags + SQLite Include
CXXFLAGS := -std=c++17 -fvisibility=hidden -g -Wall -fPIC -Wextra -O2 \
            -DETCS_MODULE_NAME=\"$(TARGET_BASE_NAME)\" \
            -I. -I../.. -I$(SQLITE_DIR) -pipe -fno-plt $(CUSTOM_CXXFLAGS)

ifeq ($(UNAME_S),Linux)
    PLATFORM_DIR := Linux
    LDFLAGS := -shared -pthread -fuse-ld=gold -Wl,--threads \
               -Wl,--thread-count,$(shell nproc) -ldl \
               -Wl,--version-script=exports.map
    FINAL_TARGET := $(TARGET_BASE_NAME).so
else ifeq ($(UNAME_S),Windows_NT)
    PLATFORM_DIR := Win
    LDFLAGS := -shared -lws2_32 
    FINAL_TARGET := $(TARGET_BASE_NAME).dll
else
    $(error Unsupported platform: $(UNAME_S))
endif

# -------------------------
# Headers
LOCAL_HEADERS := $(wildcard $(PLATFORM_DIR)/*.h) $(wildcard $(TARGET_BASE_NAME)/*.h)
MODULE_HEADERS := $(sort $(LOCAL_HEADERS) $(TARGET_BASE_NAME).h)
MODULE_DEPS := $(SRC_MODULE) $(MODULE_HEADERS) $(GLOBAL_HEADERS)

# -------------------------
# Build Targets
.PHONY: all setup_sqlite
all: setup_sqlite $(FINAL_TARGET)
	@echo "✓ Built $(FINAL_TARGET) with SQLite for $(UNAME_S)"

# 1. Ensure SQLite repo exists and generate amalgamation
setup_sqlite:
	@if [ ! -d "$(SQLITE_DIR)" ]; then \
		echo " cloning sqlite mirror..."; \
		git clone --depth 1 https://github.com/sqlite/sqlite.git $(SQLITE_DIR); \
	fi
	@if [ ! -f "$(SQLITE_AMALGAMATION)" ]; then \
		echo " generating sqlite amalgamation..."; \
		cd $(SQLITE_DIR) && ./configure && make sqlite3.c; \
	fi

# 2. Compile SQLite object (C)
$(SQLITE_OBJ): $(SQLITE_AMALGAMATION)
	$(CC) -O2 -fPIC $(SQLITE_FLAGS) -c $(SQLITE_AMALGAMATION) -o $@

# 3. Generate ACE Registration Hashes
$(HASH_HEADER): $(MODULE_HEADERS)
	@echo "// Generated Registration - do not edit" > $@
	@for f in $(MODULE_HEADERS); do \
		HASH=$$(cat $$f | openssl dgst -sha256 | awk '{print $$NF}'); \
		FULL_NAME=$$(basename $$f); \
		VAR_NAME=$$(echo $$FULL_NAME | sed 's/\./_/g'); \
		echo "inline const bool _reg_$$VAR_NAME = []() { \
			ETCS::FlatMap<ETCS::Buffer, ETCS::Buffer>::setArena(&ETCS::MemoryArena::getInstance()); \
			ETCS::Entity::getManifest()[\"$$FULL_NAME\"] = \"$$HASH\"; \
			return true; \
		}();" >> $@; \
	done

# 4. Final Link (Module + SQLite)
$(FINAL_TARGET): $(MODULE_DEPS) $(HASH_HEADER) $(SQLITE_OBJ)
	$(CXX) $(CXXFLAGS) $(EXTRADEFINES) $(LDFLAGS) -o $@ $(SRC_MODULE) $(SQLITE_OBJ)

# -------------------------
# Clean
.PHONY: clean
clean:
	rm -f $(FINAL_TARGET) *.o $(PLATFORM_DIR)/*.hash $(HASH_HEADER)
	# Optional: add 'rm -rf $(SQLITE_DIR)' if you want a total wipe
