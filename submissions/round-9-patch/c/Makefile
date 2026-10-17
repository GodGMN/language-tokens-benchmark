CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -pedantic
LDFLAGS ?=

TARGET = patch
OBJS = patch.o json.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

patch.o: patch.c json.h
	$(CC) $(CFLAGS) -c -o $@ patch.c

json.o: json.c json.h
	$(CC) $(CFLAGS) -c -o $@ json.c

selftest: $(TARGET)
	./selftest/run.sh

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all selftest clean
