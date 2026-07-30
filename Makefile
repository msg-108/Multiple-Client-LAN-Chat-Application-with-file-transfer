CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -pthread

TARGETS = server client test_utils
OBJS = utils.o

all: $(TARGETS)

utils.o: utils.c utils.h
	$(CC) $(CFLAGS) -c utils.c

server: server.c utils.o
	$(CC) $(CFLAGS) -o server server.c utils.o

client: client.c utils.o
	$(CC) $(CFLAGS) -o client client.c utils.o

test_utils: utils.c utils.h
	$(CC) $(CFLAGS) -DTEST_UTILS -o test_utils utils.c

clean:
	rm -f $(TARGETS) *.o test_input.bin test_output.bin

.PHONY: all clean
