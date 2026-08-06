CC ?= gcc
CFLAGS = -Wall -Wextra -std=c11 -pthread
LDFLAGS =

# Detect OS for cross-platform library flags
ifeq ($(OS),Windows_NT)
    LDFLAGS += -lws2_32
    TARGET_EXT = .exe
else
    UNAME_S := $(shell uname -s 2>/dev/null || echo Unknown)
    ifneq ($(findstring MINGW,$(UNAME_S)),)
        LDFLAGS += -lws2_32
        TARGET_EXT = .exe
    else ifneq ($(findstring CYGWIN,$(UNAME_S)),)
        LDFLAGS += -lws2_32
        TARGET_EXT = .exe
    else ifneq ($(findstring MSYS,$(UNAME_S)),)
        LDFLAGS += -lws2_32
        TARGET_EXT = .exe
    else
        TARGET_EXT =
    endif
endif

TARGETS = server$(TARGET_EXT) client$(TARGET_EXT) test_utils$(TARGET_EXT)
OBJS = utils.o

all: $(TARGETS)

utils.o: utils.c utils.h
	$(CC) $(CFLAGS) -c utils.c

server$(TARGET_EXT): server.c utils.o
	$(CC) $(CFLAGS) -o server$(TARGET_EXT) server.c utils.o $(LDFLAGS)

client$(TARGET_EXT): client.c utils.o
	$(CC) $(CFLAGS) -o client$(TARGET_EXT) client.c utils.o $(LDFLAGS)

test_utils$(TARGET_EXT): utils.c utils.h
	$(CC) $(CFLAGS) -DTEST_UTILS -o test_utils$(TARGET_EXT) utils.c $(LDFLAGS)

clean:
	rm -rf $(TARGETS) *.o *.dSYM test_input.bin test_output.bin server client test_utils server.exe client.exe test_utils.exe

.PHONY: all clean

