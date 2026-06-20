CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
TARGET := vimish
SRC := main.c

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
