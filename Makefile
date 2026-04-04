CC = gcc
CFLAGS = -Wall -Wextra -O2
LIBS = -lncurses -lutil -lcurl -lpthread
TARGET = cursory
SRC = main.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) $(LIBS) -o $(TARGET)

clean:
	rm -f $(TARGET)
	rm -f cursory.log

run: all
	./$(TARGET)

.PHONY: all clean run
