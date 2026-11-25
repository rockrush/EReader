CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=gnu11

SRC := $(wildcard *.c)

init: $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -static

.PHONY: clean
clean:
	@rm -f init
