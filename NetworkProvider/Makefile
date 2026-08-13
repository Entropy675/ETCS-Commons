# ---------------------------------------------------------
TARGET_BASE_NAME := $(notdir $(CURDIR))

GLOBAL_HEADERS := ../../ontology.h ../../ontology_hashes.h ../../libs.h ../../libs_hashes.h ../../core_defs.h

PICO_DIR := picohttpparser
PICO_OBJ := picohttpparser.o

MBEDTLS_DIR := mbedtls
MBEDTLS_BUILD := $(MBEDTLS_DIR)/build
MBEDTLS_LIB := $(MBEDTLS_BUILD)/library/libmbedtls.a \
               $(MBEDTLS_BUILD)/library/libmbedcrypto.a \
               $(MBEDTLS_BUILD)/library/libmbedx509.a

ARCH := $(shell uname -m)
ARCH_MARKER := $(MBEDTLS_BUILD)/.arch_$(ARCH)

SRC_MODULE := $(TARGET_BASE_NAME).cc
HASH_HEADER := module_hashes.h

UNAME_S := $(shell uname -s)
CXX := g++
CC  := gcc

CXXFLAGS := -std=c++17 -fvisibility=hidden -g -Wall -fPIC -Wextra -O2 \
            -DETCS_MODULE_NAME=\"$(TARGET_BASE_NAME)\" \
            -I. -I../.. -I$(PICO_DIR) \
            -I$(MBEDTLS_DIR)/include \
            -I$(MBEDTLS_DIR)/tf-psa-crypto/include \
            -I$(MBEDTLS_DIR)/tf-psa-crypto/drivers/builtin/include \
            -I$(MBEDTLS_DIR)/tf-psa-crypto/drivers/everest/include \
            -I$(MBEDTLS_DIR)/tf-psa-crypto/drivers/pqcp/include \
            -I$(MBEDTLS_DIR)/tf-psa-crypto/drivers/p256-m \
            -pipe -fno-plt $(CUSTOM_CXXFLAGS)

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

LOCAL_HEADERS := $(wildcard $(PLATFORM_DIR)/*.h) $(wildcard $(TARGET_BASE_NAME)/*.h)
MODULE_HEADERS := $(sort $(LOCAL_HEADERS) $(TARGET_BASE_NAME).h)
MODULE_DEPS := $(SRC_MODULE) $(MODULE_HEADERS) $(GLOBAL_HEADERS)

.PHONY: all setup_pico _ensure_mbedtls_cloned clean

all: setup_pico $(FINAL_TARGET)
	@echo "✓ Built $(FINAL_TARGET) with picohttpparser + mbedTLS for $(UNAME_S) ($(ARCH))"

setup_pico:
	@if [ ! -d "$(PICO_DIR)" ]; then \
	    echo " cloning picohttpparser..."; \
	    git clone --depth 1 https://github.com/h2o/picohttpparser.git $(PICO_DIR); \
	fi

$(PICO_OBJ): $(PICO_DIR)/picohttpparser.c
	$(CC) -O2 -fPIC -c $< -o $@

# --- mbedTLS (Architecture-Aware Dependency Handling) ---
$(MBEDTLS_LIB): $(ARCH_MARKER)

$(ARCH_MARKER): | _ensure_mbedtls_cloned
	@echo " configuring/building mbedtls for $(ARCH)..."
	@rm -rf $(MBEDTLS_BUILD)
	@cmake -S $(MBEDTLS_DIR) -B $(MBEDTLS_BUILD) \
	    -DENABLE_TESTING=OFF \
	    -DENABLE_PROGRAMS=OFF \
	    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
	@cmake --build $(MBEDTLS_BUILD) --parallel $(shell nproc)
	@touch $@

_ensure_mbedtls_cloned:
	@if [ ! -d "$(MBEDTLS_DIR)" ]; then \
	    echo " cloning mbedtls..."; \
	    git clone --depth 1 --recurse-submodules https://github.com/Mbed-TLS/mbedtls.git $(MBEDTLS_DIR); \
	fi

# --- Hash generation (Single-line printf to avoid tab-mangling) ---
$(HASH_HEADER): $(MODULE_HEADERS)
	@echo "// Generated Registration - do not edit" > $@
	@for f in $(MODULE_HEADERS); do \
	    HASH=$$(cat $$f | openssl dgst -sha256 | awk '{print $$NF}'); \
	    FULL_NAME=$$(basename $$f); \
	    VAR_NAME=$$(echo $$FULL_NAME | sed 's/\./_/g'); \
	    printf 'inline const bool _reg_%s = []() { ETCS::FlatMap<ETCS::Buffer, ETCS::Buffer>::setArena(&ETCS::MemoryArena::getInstance()); ETCS::Entity::getManifest()["%s"] = "%s"; return true; }();\n' "$$VAR_NAME" "$$FULL_NAME" "$$HASH" >> $@; \
	done

# --- Final link ---
$(FINAL_TARGET): $(MODULE_DEPS) $(HASH_HEADER) $(PICO_OBJ) $(MBEDTLS_LIB)
	$(CXX) $(CXXFLAGS) $(EXTRADEFINES) $(LDFLAGS) -o $@ $(SRC_MODULE) $(PICO_OBJ) \
	    $(MBEDTLS_BUILD)/library/libmbedtls.a \
	    $(MBEDTLS_BUILD)/library/libmbedx509.a \
	    $(MBEDTLS_BUILD)/library/libmbedcrypto.a

clean:
	rm -f $(FINAL_TARGET) *.o $(PLATFORM_DIR)/*.hash $(HASH_HEADER)
	# Optional: rm -rf $(PICO_DIR) $(MBEDTLS_DIR)
