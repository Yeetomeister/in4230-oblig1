CC = gcc
CFLAGS = -Wall -Wextra -std=c11
TARGETS = mipd ping_client
.PHONY: all clean

all: $(TARGETS)

mipd: src/mipd.o
	$(CC) $(CFLAGS) -o mipd src/mipd.o

ping_client: src/ping_client.o
	$(CC) $(CFLAGS) -o ping_client src/ping_client.o

src/mipd.o: src/mipd.c include/mipd.h
	$(CC) $(CFLAGS) -c src/mipd.c -o src/mipd.o

src/ping_client: src/ping_client.c
	$(CC) $(CFLAGS) -c src/ping_client -o src/ping_client.o

clean:
	rm -f $(TARGETS) src/mipd.o src/ping_client.o
