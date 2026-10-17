CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra
CPPFLAGS += -Isrc
LDFLAGS ?=
LDLIBS  ?= -lm

TARGET  := workflow
SRCS    := src/json.c src/engine.c
OBJS    := $(SRCS:.c=.o)

.PHONY: all clean selftest

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

src/%.o: src/%.c src/json.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

selftest: $(TARGET)
	./selftest/run.sh

clean:
	rm -f $(TARGET) $(OBJS)
