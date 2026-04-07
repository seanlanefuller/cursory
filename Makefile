CC = gcc
CFLAGS = -Wall -Wextra -O2
LIBS = -lncurses -lutil -lcurl -lpthread

SRCS = main.c ui.c ai.c utils.c
OBJS = $(SRCS:.c=.o)

all: cursory

cursory: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

%.o: %.c cursory.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f cursory $(OBJS) cursory.log

test: tests/test_json tests/integration tests/test_patch
	./tests/test_json
	./tests/integration
	./tests/test_patch

tests/test_patch: tests/test_patch.c utils.c
	$(CC) $(CFLAGS) -o $@ tests/test_patch.c utils.c $(LIBS)

tests/test_json: tests/test_json.c utils.c
	$(CC) $(CFLAGS) -o $@ tests/test_json.c utils.c $(LIBS)

tests/integration: tests/integration.c utils.c
	$(CC) $(CFLAGS) -o $@ tests/integration.c utils.c $(LIBS)
