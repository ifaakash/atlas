# Atlas text editor — modular build
.PHONY: all clean

CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -g

SRCS = main.c terminal.c buffer.c display.c editor.c file.c search.c syntax.c select.c config.c
OBJS = $(SRCS:.c=.o)

all: atlas

atlas: $(OBJS)
	$(CC) $(CFLAGS) -o atlas $(OBJS)

%.o: %.c atlas.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f atlas $(OBJS) *.dSYM
