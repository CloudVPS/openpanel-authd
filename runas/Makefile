all: runas

clean:
	rm -f runas runas.o

runas: runas.o
	$(CC) $(LDFLAGS) -o runas runas.o

runas.o: runas.c
	$(CC) $(CFLAGS) -c runas.c
