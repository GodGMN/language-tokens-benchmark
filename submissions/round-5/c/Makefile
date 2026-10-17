CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?=

BIN := workflow

all: $(BIN)

$(BIN): workflow.c
	$(CC) $(CFLAGS) -o $(BIN) workflow.c $(LDFLAGS)

clean:
	rm -f $(BIN)

.PHONY: all clean
