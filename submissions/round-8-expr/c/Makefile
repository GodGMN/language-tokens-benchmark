CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
TARGET   = expr

all: $(TARGET)

$(TARGET): expr.c
	$(CC) $(CFLAGS) -o $(TARGET) expr.c

clean:
	rm -f $(TARGET)

.PHONY: all clean
