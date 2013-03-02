/*
Main program for the virtual memory project.
Make all of your modifications to this file.
You may add or rearrange any code or data as you need.
The header files page_table.h and disk.h explain
how to use the page table and disk interfaces.
*/

#include "page_table.h"
#include "disk.h"
#include "program.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

unsigned int equal_pages_and_frames = 0;

struct stats {
    int page_faults;
    int disk_reads;
    int disk_writes;
};
struct stats stats;

void print_stats();


void page_fault_handler( struct page_table *pt, int page )
{
    if (equal_pages_and_frames) {
        page_table_set_entry(pt, page, page, PROT_READ | PROT_WRITE);
        ++stats.page_faults;
    } else {
        printf("unhandled page fault on page #%d\n",page);
        exit(1);
    }
}

int main( int argc, char *argv[] )
{
	if(argc!=5) {
		printf("use: virtmem <npages> <nframes> <rand|fifo|custom> <sort|scan|focus>\n");
		return 1;
	}

	int npages = atoi(argv[1]);
	int nframes = atoi(argv[2]);
	const char *program = argv[4];

    if (npages == nframes) equal_pages_and_frames = 1;
    memset(&stats, 0, sizeof(struct stats));

	struct disk *disk = disk_open("myvirtualdisk",npages);
	if(!disk) {
		fprintf(stderr,"couldn't create virtual disk: %s\n",strerror(errno));
		return 1;
	}


	struct page_table *pt = page_table_create( npages, nframes, page_fault_handler );
	if(!pt) {
		fprintf(stderr,"couldn't create page table: %s\n",strerror(errno));
		return 1;
	}

	char *virtmem = page_table_get_virtmem(pt);

	//char *physmem = page_table_get_physmem(pt);

	if(!strcmp(program,"sort")) {
		sort_program(virtmem,npages*PAGE_SIZE);

	} else if(!strcmp(program,"scan")) {
		scan_program(virtmem,npages*PAGE_SIZE);

	} else if(!strcmp(program,"focus")) {
		focus_program(virtmem,npages*PAGE_SIZE);

	} else {
		fprintf(stderr,"unknown program: %s\n",argv[3]);

	}

	page_table_delete(pt);
	disk_close(disk);

    print_stats(&stats);

	return 0;
}


void print_stats() {
    printf("\nStatistics:\n");
    printf("=====================\n");
    printf("Page faults = %d\n", stats.page_faults);
    printf("Disk reads  = %d\n", stats.disk_reads);
    printf("Disk writes = %d\n", stats.disk_writes);
}

