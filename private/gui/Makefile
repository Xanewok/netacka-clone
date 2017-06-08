SRC=cmd.c err.c net.c read_line.c

CC = gcc

CFLAGS=-std=c99 -Wall -Wunused
#CFLAGS=-std=c99 -Wall -Wunused -DDEBUG

gui1: gui1.c $(SRC) gui.h 
	$(CC) $(CFLAGS) gui1.c $(SRC) -o gui1 `pkg-config gtk+-2.0 --cflags --libs`

clean: 
	rm -f *.o gui1
