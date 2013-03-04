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
#include <time.h>

#define DEBUG

struct disk *disk = NULL;
char *virtmem = NULL;
char *physmem = NULL;

// Each entry corresponds to a single frame in physical memory
//  - zero values indicate that the indexed frame is available
//  - non-zero values indicate that the indexed frame is in use
unsigned char *frame_table = NULL;

// Statistics -----------------------------------------------------------------
struct stats {
    int page_faults;
    int disk_reads;
    int disk_writes;
    int evictions;
};
struct stats stats;

void print_stats();

// Program arguments ----------------------------------------------------------
struct args {
    int npages;
    int nframes;
    const char *policy;
    const char *program;
};
struct args args;


// Page fault handling policies and handler functions -------------------------
enum policy_e { RAND, FIFO, TWO_FIFO, CUSTOM };
enum policy_e fault_policy;

void page_fault_handler_rand( struct page_table *pt, int page );
void page_fault_handler_fifo( struct page_table *pt, int page );
void page_fault_handler_2fifo( struct page_table *pt, int page );
void page_fault_handler_custom( struct page_table *pt, int page );

int find_free_frame();


// FIFO list ------------------------------------------------------------------
struct list_node {
    int frame_index;
    struct list_node *next;
};
struct list_node *fifo_head = NULL;
struct list_node *fifo_tail = NULL;

void fifo_insert(int frame_index);
int  fifo_remove();
void fifo_print();
void fifo_free();


// Generic page fault handler -------------------------------------------------
void page_fault_handler( struct page_table *pt, int page )
{
    ++stats.page_faults;

    if (args.npages == args.nframes) {
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

    memset(&args, 0, sizeof(struct args));
    args.npages  = atoi(argv[1]);
    args.nframes = atoi(argv[2]);
    args.policy  = argv[3];
    args.program = argv[4];

    srand48((long int) time(NULL));

    // Set page fault handling policy
         if (!strcmp(args.policy,"rand"))   fault_policy = RAND;
    else if (!strcmp(args.policy,"fifo"))   fault_policy = FIFO;
    else if (!strcmp(args.policy,"2fifo"))  fault_policy = TWO_FIFO;
    else if (!strcmp(args.policy,"custom")) fault_policy = CUSTOM;
    else {
		printf("use: virtmem <npages> <nframes> <rand|fifo|2fifo|custom> <sort|scan|focus>\n");
		return 1;
    }

    // Setup frame table and statistics
    frame_table = malloc(args.nframes * sizeof(unsigned char));
    memset(frame_table, 0, args.nframes * sizeof(unsigned char));
    memset(&stats, 0, sizeof(struct stats));

    // Initialize disk
	disk = disk_open("myvirtualdisk",args.npages);
	if(!disk) {
		fprintf(stderr,"couldn't create virtual disk: %s\n",strerror(errno));
		return 1;
	}

    // Initialize page table
	struct page_table *pt = page_table_create( args.npages, args.nframes, page_fault_handler );
	if(!pt) {
		fprintf(stderr,"couldn't create page table: %s\n",strerror(errno));
		return 1;
	}

	virtmem = page_table_get_virtmem(pt);
	physmem = page_table_get_physmem(pt);

    // Run a program
	     if(!strcmp(args.program,"sort"))  sort_program(virtmem,args.npages*PAGE_SIZE);
	else if(!strcmp(args.program,"scan"))  scan_program(virtmem,args.npages*PAGE_SIZE);
	else if(!strcmp(args.program,"focus")) focus_program(virtmem,args.npages*PAGE_SIZE);
	else {
		fprintf(stderr,"unknown program: %s\n", args.program);
	}

    // Cleanup
    free(frame_table);
	page_table_delete(pt);
	disk_close(disk);

    // Results
    print_stats(&stats);

	return 0;
}


// ----------------------------------------------------------------------------
void page_fault_handler_rand( struct page_table *pt, int page ) {
    int frame, bits;
    page_table_get_entry(pt, page, &frame, &bits);

    // Update protection bits and find the frame index for page loading
    int frame_index = -1;
    if (!bits) { // Missing read bit
        bits |= PROT_READ;
        if ((frame_index = find_free_frame()) < 0) {
            // No free frames available, evict a random frame's page
            // and use that frame to load the new page
            frame_index = (int) lrand48() % args.nframes;
#ifdef DEBUG
            printf("Evicting page in frame #%d for page #%d\n", frame_index, page);
#endif
            disk_write(disk, page, &physmem[frame_index * PAGE_SIZE]);
            ++stats.disk_writes;
            ++stats.evictions;
        }
    } else if (bits & PROT_READ && !(bits & PROT_WRITE)) { // Missing write bit
        bits |= PROT_WRITE;
        frame_index = frame;
    } else { // Shouldn't get here...
        printf("Warning: entered page fault handler for page with all protection bits enabled\n");
        return;
    }

    // Update the page table entry for this page
    page_table_set_entry(pt, page, frame_index, bits);

    // Mark the frame as used
    frame_table[frame_index] = 1;

    // Read in from disk to physical memory
    disk_read(disk, page, &physmem[frame_index * PAGE_SIZE]);
    ++stats.disk_reads;

#ifdef DEBUG
    printf("Setting page %d to frame %d\n", page, frame_index);
    page_table_print_entry(pt, page);
    puts("");
#endif
}


// ----------------------------------------------------------------------------
void page_fault_handler_fifo( struct page_table *pt, int page ) {
    int frame, bits;
    page_table_get_entry(pt, page, &frame, &bits);

    // Update protection bits and find the frame index for page loading
    int frame_index = -1;
    if (!bits) { // Missing read bit
        bits |= PROT_READ;
        if ((frame_index = find_free_frame()) < 0) {
            // Evict page from tail of queue
            if ((frame_index = fifo_remove()) < 0) {
                printf("Warning: attempted to remove frame index from empty fifo!\n");
                return;
            }
#ifdef DEBUG
            printf("Evicting page in frame #%d for page #%d\n", frame_index, page);
#endif
            disk_write(disk, page, &physmem[frame_index * PAGE_SIZE]);
            ++stats.disk_writes;
            ++stats.evictions;
        }
    } else if (bits & PROT_READ && !(bits & PROT_WRITE)) { // Missing write bit
        bits |= PROT_WRITE;
        frame_index = frame;
    } else { // Shouldn't get here?
        printf("Warning: entered page fault handler for page with all protection bits enabled\n");
        return;
    }

    // Update the page table entry for this page
    page_table_set_entry(pt, page, frame_index, bits);

    // Mark the frame as used and insert it into the fifo
    frame_table[frame_index] = 1;
    fifo_insert(frame_index);

    // Read in from disk to physical memory
    disk_read(disk, page, &physmem[frame_index * PAGE_SIZE]);
    ++stats.disk_reads;

#ifdef DEBUG
    printf("Set page %d to frame %d\n", page, frame_index);
    page_table_print_entry(pt, page);
    puts("");
#endif
}


// ----------------------------------------------------------------------------
void page_fault_handler_2fifo( struct page_table *pt, int page ) {
    // TODO
    printf("TWO_FIFO: unhandled page fault on page #%d\n",page);
    exit(1);
}


// ----------------------------------------------------------------------------
void page_fault_handler_custom( struct page_table *pt, int page ) {
    // TODO
    printf("CUSTOM: unhandled page fault on page #%d\n",page);
    exit(1);
}

int find_free_frame() {
    // Search frame table for a free (unused) frame
    int i = 0;
    for (; i < args.nframes; ++i) {
        if (frame_table[i] == 0)
            break;
    }

    // If a free frame was found, return its index; otherwise return error code
    return (i < args.nframes) ? i : -1;
}

/**
 * Create and insert a new node with the specified frame_index into the fifo list
 **/
void fifo_insert(int frame_index) {
#ifdef DEBUG
    printf("fifo_insert(): inserting frame #%d", frame_index);
    fifo_print();
#endif

    struct list_node *node = malloc(sizeof(struct list_node));
    node->frame_index = frame_index;
    node->next = NULL;

    // Insert frame_index into fifo at tail (making it the new tail)
    if (fifo_tail == NULL) { // No nodes in list
        fifo_head = node;
        fifo_tail = node;

#ifdef DEBUG
        printf("fifo_insert(): inserted\t\t");
        fifo_print();
#endif
    } else {                 // Nodes in list
        // See if the frame_index is already in the list
        struct list_node *n = fifo_tail;
        while (n != NULL) {
            if (frame_index == n->frame_index) break;
            else                               n = n->next;
        }

        // Don't insert already inserted items
        if (n != NULL) {
#ifdef DEBUG
            printf("Node for frame #%d already in list, skipping insertion\n", frame_index);
#endif
            free(node);
            return;
        }

        node->next = fifo_tail;
        fifo_tail  = node;

#ifdef DEBUG
        printf("fifo_insert(): inserted\t\t");
        fifo_print();
#endif
    }
}

/**
 * Remove the head node of the fifo list and return its frame_index value,
 * or -1 if the fifo list is empty
 **/
int fifo_remove() {
#ifdef DEBUG
    printf("fifo_remove()");
    fifo_print();
#endif

    if (fifo_head == NULL) { // Nothing to remove
#ifdef DEBUG
        printf("fifo_remove(): head == null, nothing to remove\n");
#endif
        return -1;
    } else if (fifo_head == fifo_tail) { // Only 1 element
        int frame_index = fifo_head->frame_index;
        free(fifo_head);
        fifo_head = fifo_tail = NULL;
#ifdef DEBUG
        printf("fifo_remove(): head == tail, 1 element, removing frame #%d", fifo_head->frame_index);
        fifo_print();
#endif
        return frame_index;
    }

    // Remove the head of the fifo
    struct list_node *node = fifo_tail;
    while (node != NULL && node->next != fifo_head) {
        node = node->next;
    }
    struct list_node *head = fifo_head;
    int frame_index = head->frame_index;
    fifo_head = node;
    fifo_head->next = NULL;
    free(head);

#ifdef DEBUG
    printf("fifo_remove(): removing frame #%d", frame_index);
    fifo_print();
#endif
    return frame_index;
}

/**
 * Print the contents of the fifo from tail to head
 **/
void fifo_print() {
    printf("\tFIFO: [");
    struct list_node *node = fifo_tail;
    while (node != NULL) {
        printf(" %d", node->frame_index);
        node = node->next;
    }
    printf(" ]\n");
}

/**
 * Free all the nodes in the fifo list
 **/
void fifo_free() {
    struct list_node *node = fifo_tail;
    while (node != NULL) {
        struct list_node *next = node->next;
        free(node);
        node = next;
    }
    fifo_head = fifo_tail = NULL;
}

void print_stats() {
    printf("\nStatistics:\n");
    printf("=====================\n");
    printf("Page faults = %d\n", stats.page_faults);
    printf("Disk reads  = %d\n", stats.disk_reads);
    printf("Disk writes = %d\n", stats.disk_writes);
    printf("Evictions   = %d\n", stats.evictions);
}

