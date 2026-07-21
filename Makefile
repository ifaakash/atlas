# Use of phony - This target is not a real file. Always run its commands when requested
.PHONY: main clean

# Compiler
CC = gcc

# Compiler flags 
CFLAGS= -Wall -Wextra


# Compile main.c
main:
	$(CC) $(CFLAGS) main.c -o main.o

clean:
	rm -rf *.o
