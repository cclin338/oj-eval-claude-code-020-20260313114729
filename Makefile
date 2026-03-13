.PHONY: all clean

all: code

code: main.c buddy.c buddy.h utils.h
	gcc -o code main.c buddy.c -std=c11 -O2

clean:
	rm -f code test *.o
