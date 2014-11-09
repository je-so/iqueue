OS=$(shell uname)

AR ?= ar
CC ?= gcc

CFLAGS := -Iinclude
CFLAGS += -std=c99 -pedantic -Wall -Wextra -Wconversion -Wshadow
CFLAGS += -Wcast-qual -Wwrite-strings -Wstrict-prototypes 
CFLAGS += -Wformat-nonliteral -Wformat-y2k
CFLAGS_debug   := $(CFLAGS) -g
CFLAGS_release := $(CFLAGS) -O3

LIBS := -lpthread

SRC := src/iqueue.c
OBJ_debug   := $(SRC:src/%.c=bin/debug/%.o)
OBJ_release := $(SRC:src/%.c=bin/release/%.o)

all:	info makedir iqueue test examples

clean:
	@echo clean bin/
	@rm -rf bin/ example?

info:
	@echo Building iqueue on '$(OS)'
	@echo Use "'make run'" to start iqueue test

makedir:
	@mkdir -p bin/release bin/debug

iqueue: bin/iqueue.a bin/iqueue_debug.a

test: bin/iqueue_test bin/iqueue_test_debug

examples: example1 example2 example3 example4

run: bin/iqueue_test
	@bin/iqueue_test

bin/iqueue_test: src/test.c bin/iqueue.a
	@echo $(CC) $^ $(LIBS) -o $@
	@$(CC) $(CFLAGS_release) $^ $(LIBS) -o $@

bin/iqueue_test_debug: src/test.c bin/iqueue_debug.a
	@echo $(CC) $^ $(LIBS) -o $@
	@$(CC) $(CFLAGS_debug) $^ $(LIBS) -o $@

example%: example%.c bin/iqueue.a
	@echo $(CC) $^ $(LIBS) -o $@
	@$(CC) $(CFLAGS_release) $^ $(LIBS) -o $@

bin/iqueue.a: $(OBJ_release)
	$(AR) rcs $@ $^

bin/iqueue_debug.a: $(OBJ_debug)
	$(AR) rcs $@ $^

bin/release/%.o: src/%.c $(wildcard include/*)
	@echo $(CC) -c $< -o $@
	@$(CC) $(CFLAGS_release) -c $< -o $@

bin/debug/%.o: src/%.c $(wildcard include/*)
	@echo $(CC) -c $< -o $@
	@$(CC) $(CFLAGS_debug) -c $< -o $@
