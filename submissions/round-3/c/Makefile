CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -pedantic
LDFLAGS ?=

TARGET = workflow
SRC    = workflow.c

.PHONY: all clean selftest

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)

selftest: $(TARGET)
	@sh selftest/run.sh
