MAKEFLAGS += -r -R

SRC := ../src
CC := cc
CFLAGS := -O2 -Wall -I$(SRC)
TEST_SRCS := tests/harness.c tests/params.c $(SRC)/helios/heliosHistory.c $(SRC)/helios/heliosFusionPredictor.c

.PHONY: all build test clean

all: build test

build:
	scripts/build.sh

test: tests/harness
	tests/harness

tests/harness: $(TEST_SRCS) $(wildcard $(SRC)/helios/*.h)
	$(CC) $(CFLAGS) -o $@ $(TEST_SRCS)

clean:
	rm -f tests/harness
