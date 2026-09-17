# Builds the firmware's C code into a shared library the host demo can call.
FW      := ../../firmware/voice_node
OUT     := ../out/libkwssim.so
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O2 -fPIC

$(OUT): bridge.c $(FW)/kws_postprocess.c $(FW)/kws_postprocess.h \
        $(FW)/kws_packet.h $(FW)/kws_config.h
	@mkdir -p ../out
	$(CC) $(CFLAGS) -I$(FW) -shared -o $@ bridge.c $(FW)/kws_postprocess.c

clean:
	rm -f $(OUT)

.PHONY: clean
