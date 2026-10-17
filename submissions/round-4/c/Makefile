CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
LDFLAGS ?=

.PHONY: all clean selftest

all: workflow

workflow: workflow.c
	$(CC) $(CFLAGS) -o $@ workflow.c $(LDFLAGS)

selftest: workflow
	@sh selftest/run.sh

clean:
	rm -f workflow
