CC = gcc
CFLAGS = -Wall -Wextra -std=c11
TARGET = mipd
.PHONY: all clean

all: $(TARGET)

$(TARGET): src/mipd.o
	$(CC) $(CFLAGS) -o $(TARGET) src/mipd.o

src/mipd.o: src/mipd.c include/mipd.h
	$(CC) $(CFLAGS) -c src/mipd.c -o src/mipd.o

clean:
	rm -f $(TARGET) src/mipd.o
