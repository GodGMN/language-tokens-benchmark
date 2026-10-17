CC ?= cc
CFLAGS ?= -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter
OBJS = json.o workflow.o

all: workflow

workflow: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

json.o: json.c json.h
	$(CC) $(CFLAGS) -c json.c

workflow.o: workflow.c json.h
	$(CC) $(CFLAGS) -c workflow.c

test: workflow
	./selftest/run.sh

clean:
	rm -f $(OBJS) workflow

.PHONY: all test clean
