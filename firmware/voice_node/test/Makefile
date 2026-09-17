# Host build of the shared decision-logic and packet tests. No ESP toolchain.
CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O2
LDLIBS  := -lm

TESTS := test_postprocess test_packet

all: run

test_postprocess: test_postprocess.c ../kws_postprocess.c ../kws_postprocess.h \
                  ../kws_config.h kws_test_vectors.h
	$(CC) $(CFLAGS) -o $@ test_postprocess.c ../kws_postprocess.c $(LDLIBS)

test_packet: test_packet.c ../kws_packet.h
	$(CC) $(CFLAGS) -o $@ test_packet.c $(LDLIBS)

run: $(TESTS)
	@for t in $(TESTS); do echo; ./$$t || exit 1; done

clean:
	rm -f $(TESTS)

.PHONY: all run clean
