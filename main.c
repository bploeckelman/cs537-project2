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

struct disk *disk = NULL;

// Statistics -----------------------------------------------------------------
struct stats {
    int page_faults;
    int disk_reads;
    int disk_writes;
};
struct stats stats;

void print_stats();

// Page fault handling policies and handler functions -------------------------
enum policy_e { RAND, FIFO, TWO_FIFO, CUSTOM };
enum policy_e fault_policy;

void page_fault_handler_rand( struct page_table *pt, int page );
void page_fault_handler_fifo( struct page_table *pt, int page );
void page_fault_handler_2fifo( struct page_table *pt, int page );
void page_fault_handler_custom( struct page_table *pt, int page );


// Generic page fault handler -------------------------------------------------
void page_fault_handler( struct page_table *pt, int page )
{
    ++stats.page_faults;

    if (equal_pages_and_frames) {
        // Handle simple case of direct mapping between pages and frames
        page_table_set_entry(pt, page, page, PROT_READ | PROT_WRITE);
    } else {
        // Delegate to appropriate page handler for active policy
        switch (fault_policy) {
            case RAND:      page_fault_handler_rand(pt, page);   break;
            case FIFO:      page_fault_handler_fifo(pt, page);   break;
            case TWO_FIFO:  page_fault_handler_2fifo(pt, page);  break;
            case CUSTOM:    page_fault_handler_custom(pt, page); break;
            default:
            {
                printf("unhandled page fault on page #%d\n",page);
                exit(1);
            }
        }
    }
}


// Entry point ----------------------------------------------------------------
int main( int argc, char *argv[] )
{
	if(argc!=5) {
		printf("use: virtmem <npages> <nframes> <rand|fifo|2fifo|custom> <sort|scan|focus>\n");
		return 1;
	}

	int npages = atoi(argv[1]);
	int nframes = atoi(argv[2]);
    const char *policy = argv[3];
	const char *program = argv[4];

    // Set page fault handling policy
         if (!strcmp(policy,"rand"))   fault_policy = RAND;
    else if (!strcmp(policy,"fifo"))   fault_policy = FIFO;
    else if (!strcmp(policy,"2fifo"))  fault_policy = TWO_FIFO;
    else if (!strcmp(policy,"custom")) fault_policy = CUSTOM;
    else {
		printf("use: virtmem <npages> <nframes> <rand|fifo|2fifo|custom> <sort|scan|focus>\n");
		return 1;
    }

    // Check for simple 
    if (npages == nframes) equal_pages_and_frames = 1;
    memset(&stats, 0, sizeof(struct stats));

    // Initialize disk
	disk = disk_open("myvirtualdisk",npages);
	if(!disk) {
		fprintf(stderr,"couldn't create virtual disk: %s\n",strerror(errno));
		return 1;
	}

    // Initialize page table
	struct page_table *pt = page_table_create( npages, nframes, page_fault_handler );
	if(!pt) {
		fprintf(stderr,"couldn't create page table: %s\n",strerror(errno));
		return 1;
	}

	char *virtmem = page_table_get_virtmem(pt);

	//char *physmem = page_table_get_physmem(pt);

    // Run a program
	     if(!strcmp(program,"sort"))  sort_program(virtmem,npages*PAGE_SIZE);
	else if(!strcmp(program,"scan"))  scan_program(virtmem,npages*PAGE_SIZE);
	else if(!strcmp(program,"focus")) focus_program(virtmem,npages*PAGE_SIZE);
	else {
		fprintf(stderr,"unknown program: %s\n",argv[3]);
	}

    // Cleanup
	page_table_delete(pt);
	disk_close(disk);

    // Results
    print_stats(&stats);

	return 0;
}


// ----------------------------------------------------------------------------
void page_fault_handler_rand( struct page_table *pt, int page ) {
    // TODO
    printf("RAND: unhandled page fault on page #%d\n",page);
    exit(1);
}

void page_fault_handler_fifo( struct page_table *pt, int page ) {
    // TODO
    printf("FIFO: unhandled page fault on page #%d\n",page);
    exit(1);
}

void page_fault_handler_2fifo( struct page_table *pt, int page ) {
    // TODO
    printf("TWO_FIFO: unhandled page fault on page #%d\n",page);
    exit(1);
}

void page_fault_handler_custom( struct page_table *pt, int page ) {
    // TODO
    printf("CUSTOM: unhandled page fault on page #%d\n",page);
    exit(1);
}


void print_stats() {
    printf("\nStatistics:\n");
    printf("=====================\n");
    printf("Page faults = %d\n", stats.page_faults);
    printf("Disk reads  = %d\n", stats.disk_reads);
    printf("Disk writes = %d\n", stats.disk_writes);
}

