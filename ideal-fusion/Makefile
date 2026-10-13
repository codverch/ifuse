# Build and test Scarab with ideal fusion. Run from this directory.
#
#   make        patch and build Scarab in build/scarab
#   make test   patch Scarab and run tests/run.sh (no Scarab build needed)
#   make clean  remove build/
#
# The tree is recreated whenever the patch, sources, or patch script change.

TREE := build/scarab
STAMP := $(TREE)/.ideal-fusion-stamp
TARGET ?= opt

.PHONY: all
all: scarab

.PHONY: scarab
scarab: $(STAMP)
	scripts/build.sh $(TREE) $(TARGET)

$(STAMP): scarab.patch $(wildcard src/ideal-fusion/*) scripts/patch.sh
	rm -rf $(TREE)
	scripts/patch.sh $(TREE)
	touch $@

.PHONY: test
test: $(STAMP)
	tests/run.sh $(TREE)

.PHONY: clean
clean:
	rm -rf build
