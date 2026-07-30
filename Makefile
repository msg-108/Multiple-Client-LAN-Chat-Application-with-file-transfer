CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -pthread

TARGETS = server client test_utils

all: $(TARGETS)

server: server.c
	$(CC) $(CFLAGS) -o server server.c

client: client.c
	$(CC) $(CFLAGS) -o client client.c

test_utils: utils.c utils.h
	$(CC) $(CFLAGS) -DTEST_UTILS -o test_utils utils.c

clean:
	rm -f $(TARGETS) test_input.bin test_output.bin

.PHONY: all clean
