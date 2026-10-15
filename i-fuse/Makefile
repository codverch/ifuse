# Builds and tests Scarab with I-Fuse. Run make from this directory.
#
#   make        Patch and build Scarab in build/scarab
#   make test   Patch Scarab and run tests/run.sh (no Scarab build needed)
#   make clean  Remove build/
#
# Make recreates the tree whenever the patch, the sources, or the patch
# script changes.

TREE := build/scarab
STAMP := $(TREE)/.ifuse-stamp
TARGET ?= opt

.PHONY: all
all: scarab

.PHONY: scarab
scarab: $(STAMP)
	scripts/build.sh $(TREE) $(TARGET)

$(STAMP): scarab.patch $(shell find src -type f) scripts/patch.sh
	rm -rf $(TREE)
	scripts/patch.sh $(TREE)
	touch $@

.PHONY: test
test: $(STAMP)
	tests/run.sh $(TREE)

.PHONY: clean
clean:
	rm -rf build
