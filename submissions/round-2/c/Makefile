CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS ?=

SRC = src/json.c src/main.c
OBJ = $(SRC:.c=.o)
HDR = src/json.h

all: workflow

workflow: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

src/%.o: src/%.c $(HDR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

test: workflow
	./selftest/run.sh

clean:
	rm -f $(OBJ) workflow

.PHONY: all clean test
