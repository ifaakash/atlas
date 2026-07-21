# Use of phony - This target is not a real file. Always run its commands when requested
.PHONY: main clean

# Compiler
CC = gcc

# Compiler flags 
CFLAGS= -std11 -Wall -Wextra -g


# Compile main.c
main:
	$(CC) $(CFLAGS) main.c -o atlas

clean:
	rm -f *.o atlas
