# Makefile for Word Guessing Game (Server + Client)

CC = gcc
CFLAGS = -Wall -pthread

all: server client

server: server1.c
	$(CC) $(CFLAGS) -o server1 server1.c

client: client1.c
	$(CC) $(CFLAGS) -o client1 client1.c

clean:
	rm -f server client *.o