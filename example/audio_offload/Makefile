EXAMPLE_NAME = rpmsg_audio_offload_example
EXAMPLE_DIR = $(shell pwd)
SRC = $(wildcard src/*.c)
INC = -Iinc -Isrc -I../../include
OUT_DIR = bin
BIN = $(OUT_DIR)/$(EXAMPLE_NAME)

CFLAGS = -Wall
LDLIBS = -L../../../lib -lrpmsg_dma -lti_rpmsg_char -lfftw3 -lsndfile -lasound

all: dirs $(BIN)

dirs:
	@mkdir -p $(OUT_DIR)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(INC) $(LDLIBS)

clean:
	rm -rf $(OUT_DIR)

