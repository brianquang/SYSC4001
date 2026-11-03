CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
CPPFLAGS ?= -Iinclude

TARGET := scheduler
SOURCES := $(wildcard src/*.c)
OBJECTS := $(SOURCES:.c=.o)

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o $@

src/%.o: src/%.c include/scheduler.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

test: all
	python3 test_harness.py --no-compile

clean:
	rm -f $(TARGET) $(OBJECTS)
