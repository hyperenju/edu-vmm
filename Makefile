CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11

.PHONY: all run clean

all: helloworld

helloworld: helloworld.c
	$(CC) $(CFLAGS) -o $@ $<

run: helloworld
	./helloworld

clean:
	rm -f helloworld
