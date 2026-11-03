CC ?= cc
CFLAGS ?= -std=c11 -D_XOPEN_SOURCE=700 -Wall -Wextra -Wpedantic -O2 -pthread
CPPFLAGS ?= -Iinclude
LDFLAGS ?= -pthread

STUDENT_SOURCES := $(filter-out src/test_driver.c src/benchmark.c,$(wildcard src/*.c))

.PHONY: all clean

all: sync_runner sync_benchmark

sync_runner: src/test_driver.c $(STUDENT_SOURCES) include/sync.h
	$(CC) $(CPPFLAGS) $(CFLAGS) src/test_driver.c $(STUDENT_SOURCES) -o $@ $(LDFLAGS)

sync_benchmark: src/benchmark.c $(STUDENT_SOURCES) include/sync.h
	$(CC) $(CPPFLAGS) $(CFLAGS) src/benchmark.c $(STUDENT_SOURCES) -o $@ $(LDFLAGS)

ifeq ($(OS),Windows_NT)
clean:
	cmd /C "del /Q sync_runner sync_benchmark sync_runner.exe sync_benchmark.exe"
else
clean:
	rm -f sync_runner sync_benchmark sync_runner.exe sync_benchmark.exe
endif
