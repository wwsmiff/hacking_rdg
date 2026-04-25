CC=gcc
CFLAGS=$(shell pkg-config --cflags libspa-0.2 libpipewire-0.3)
LFLAGS=$(shell pkg-config --libs libpipewire-0.3)

main: main.o
	$(CC) main.o -o main $(LFLAGS)

main.o: main.c
	$(CC) $(CFLAGS) -c main.c

.PHONY: clean
clean:
	rm *.o main

.PHONY: run
run:
	./main
