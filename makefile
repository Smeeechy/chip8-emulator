CFLAGS=-std=c17 -Wall -Wextra -Werror

all:
	gcc -O1 chip8.c -o chip8 $(CFLAGS) `sdl2-config --cflags --libs`

debug:
	gcc chip8.c -g -o chip8 $(CFLAGS) `sdl2-config --cflags --libs` -DDEBUG

time:
	gcc chip8.c -o chip8 $(CFLAGS) `sdl2-config --cflags --libs` -DTIME