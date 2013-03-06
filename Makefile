CC=gcc
FLAGS=-c -ggdb3 --std=gnu99 -Wall #-Werror
TAGS=ctags -R

virtmem: main.o page_table.o disk.o program.o
	$(CC) main.o page_table.o disk.o program.o -o virtmem
	$(TAGS)

main.o: main.c
	$(CC) $(FLAGS) main.c -o main.o

page_table.o: page_table.c
	$(CC) $(FLAGS) page_table.c -o page_table.o

disk.o: disk.c
	$(CC) $(FLAGS) disk.c -o disk.o

program.o: program.c
	$(CC) $(FLAGS) program.c -o program.o


clean:
	rm -f *.o virtmem
