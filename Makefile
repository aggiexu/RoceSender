CC ?= mpicc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?= -libverbs

TARGET = rdma_pingpong
SRC = src/main.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
