# --- Master Makefile for ETCS Modules (Folder-Name Based) ---

# ====================================================================
# CONFIGURATION - Dynamic Folder Discovery
# ====================================================================

# DISCOVERY: Find all immediate subdirectories that contain a Makefile.
SUBDIRS := $(patsubst %/Makefile,%,$(wildcard */Makefile))

# Platform-specific shared library extension
ifeq ($(OS),Windows_NT)
    LIB_EXT := dll
else
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Darwin)
        LIB_EXT := dylib
    else
        LIB_EXT := so
    endif
endif

# Target directory for binaries
BIN_DIR := ../bin

# ====================================================================
# PHONY TARGETS
# ====================================================================

.PHONY: all clean $(SUBDIRS) copy_modules

# Default target
all: $(SUBDIRS) copy_modules

# ====================================================================
# BUILD RULE
# ====================================================================

$(SUBDIRS):
	@echo "\n=============================================="
	@echo "--- Building Module: $@ ---"
	@echo "=============================================="
	$(MAKE) -C $@

# ====================================================================
# COPY MODULE FILES
# Logic: Checks for folder_name.ext inside folder_name/
# ====================================================================

copy_modules:
	@mkdir -p $(BIN_DIR)
	@echo "\n=============================================="
	@echo "--- Moving outputs to $(BIN_DIR)/ ---"
	@echo "=============================================="
	@for dir in $(SUBDIRS); do \
		found=0; \
		for ext in "" ".$(LIB_EXT)"; do \
			target="$$dir/$$dir$$ext"; \
			if [ -f "$$target" ]; then \
				mv "$$target" $(BIN_DIR)/; \
				echo " [✓] Moved: $$target -> $(BIN_DIR)/"; \
				found=1; \
			fi; \
		done; \
		if [ $$found -eq 0 ]; then \
			echo " [!] Warning: No output found for $$dir (expected $$dir or $$dir.$(LIB_EXT))"; \
		fi; \
	done

# ====================================================================
# CLEANUP
# ====================================================================

clean:
	@for dir in $(SUBDIRS); do \
		$(MAKE) -C "$$dir" clean; \
		for ext in "" ".$(LIB_EXT)"; do \
			rm -f $(BIN_DIR)/$$dir$$ext; \
		done; \
	done
	@echo "\nClean complete."
