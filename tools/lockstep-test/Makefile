# The lockstep protocol, built for the host.
#
# lockstep.c is deliberately free of ESP-IDF and of Doom so that it can be run
# here: two badges on a bench cannot be made to drop frames on demand, and a
# ten-minute co-op session is not something a firmware build can assert about
# itself. See main.c.

SRC := ../../firmware/components/doom/src
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Werror -I$(SRC)

test: lockstep-test
	./lockstep-test

lockstep-test: main.c $(SRC)/lockstep.c $(SRC)/lockstep.h
	$(CC) $(CFLAGS) -o $@ main.c $(SRC)/lockstep.c

clean:
	rm -f lockstep-test

.PHONY: test clean
