all: fcat

clean:
	rm -f fcat fcat.o

fcat: fcat.o
	$(CC) $(LDFLAGS) -o fcat fcat.o

fcat.o: fcat.c
	$(CC) $(CFLAGS) -c fcat.c
