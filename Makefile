DATE ::= $(shell date --iso-8601=seconds)

GIT_INFO := $(shell git describe --abbrev=7 --dirty --always --tags)

CPPFLAGS := -DBUILD_GIT_INFO=\"$(GIT_INFO)\" -DBUILD_DATE=\"$(DATE)\"
CFLAGS := -Wall -O2
CC = gcc

all: bin/trackscreen
bin tests/bin:
	mkdir -p $@

bin/%: %.c | bin
	${CC} ${CPPFLAGS} ${CFLAGS} $^ -o $@ -ludev -lm

clean:
	rm -r bin

.PHONY: all clean
