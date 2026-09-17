# Host build of the decision-logic tests. No ESP toolchain needed.
CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O2
LDLIBS  := -lm

all: run

test_postprocess: test_postprocess.c ../kws_postprocess.c ../kws_postprocess.h \
                  ../kws_config.h kws_test_vectors.h
	$(CC) $(CFLAGS) -o $@ test_postprocess.c ../kws_postprocess.c $(LDLIBS)

run: test_postprocess
	./test_postprocess

clean:
	rm -f test_postprocess

.PHONY: all run clean
