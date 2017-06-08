SRC=cmd.c err.c net.c read_line.c

CC = gcc

CFLAGS=-std=c99 -Wall -Wunused -DDEBUG

gui3: gui3.c $(SRC) gui.h 
	$(CC) $(CFLAGS) gui3.c $(SRC) -o gui3 `pkg-config gtk+-3.0 --cflags --libs`

clean: 
	rm -f *.o gui3
