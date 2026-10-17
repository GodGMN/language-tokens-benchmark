CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
LDLIBS ?= -lm

workflow: workflow.c
	$(CC) $(CFLAGS) -o $@ workflow.c $(LDLIBS)

.PHONY: all clean test
all: workflow

clean:
	rm -f workflow

test: workflow
	@sh selftest/run.sh
