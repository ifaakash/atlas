# Compiler
CC = gcc

# Compiler flags 
CCFLAGS= -Wall -Wextra


# Compile main.c
main:
	$(CC) $(CCFLAGS) main.c -o main.o
