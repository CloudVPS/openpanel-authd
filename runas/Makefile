all: runas

clean:
	rm -f runas runas.o

fcat: fcat.o
	$(CC) $(LDFLAGS) -o runas runas.o

fcat.o: fcat.c
	$(CC) $(CFLAGS) -c runas.c
