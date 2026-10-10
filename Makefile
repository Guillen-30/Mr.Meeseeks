CC     = gcc
CFLAGS = -Wall -Wextra -std=gnu11 -pthread
TARGET = meeseeks

all: $(TARGET)

$(TARGET): meeseeks.c
	$(CC) $(CFLAGS) -o $(TARGET) meeseeks.c -lm

clean:
	rm -f $(TARGET)

.PHONY: all clean
