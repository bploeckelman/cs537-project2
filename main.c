/*
Main program for the virtual memory project.
Make all of your modifications to this file.
You may add or rearrange any code or data as you need.
The header files page_table.h and disk.h explain
how to use the page table and disk interfaces.
*/

// Authors:
// Nathan Deisinger (deisinge)
// Brian Ploeckelman (ploeckel)

// NOTICE  -----------------------------------------------------------------
// This program implements a custom algorithm which does not surpass 2FIFO
// for sort in all cases.  It does do better for cases in which we are using
// focus or scan, and in most sort cases.


#include "page_table.h"
#include "disk.h"
#include "program.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

//#define DEBUG
//#define MOVE
//#define DEBUG2
//#define RESULTS

// The below are simple ways for us to access useful data in the page table
// provided the frame number in the former three cases and the node address
// in the last.
#define PAGE(x) frame_table[x].page
#define BITS(x) frame_table[x].bits
#define FREE(x) frame_table[x].free
#define FRAMEID(x) ((int) (x - frame_table))

int FIRST_L;
int SECOND_L; //Sizes for first and second-chance lists

struct disk *disk = NULL;
char *virtmem = NULL;
char *physmem = NULL;
int f_entries = 0;
int s_entries = 0;
int chance = 0; // Used in our custom algorithm to determine where in our list
                // to remove a node.


// Statistics -----------------------------------------------------------------
struct stats {
    int page_faults;
    int disk_reads;
    int disk_writes;
    int evictions;
};
struct stats stats;

void print_stats(); // Outputs statistics slightly nicely.
void graph_stats(); // Used to output data for graphing. 

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

// Functions to help in determining where to put a new frame.
int find_free_frame();
int find_clean_frame();


// FIFO list ------------------------------------------------------------------
typedef struct _f_node {
    int page;
    int bits; // Not necessarily identical to the BITS in the page table.
    int free;
    int f_list; //0 if in none, 1 if in FIFO or first-chance, 2 if in second
    struct _f_node * next;
    struct _f_node * prev;
} f_node;

// Our page frame database.  Each entry is an above-defined f_node.
f_node *frame_table = NULL;

// The head and tail of our FIFO list.
f_node *fifo_head = NULL;
f_node *fifo_tail = NULL;

// The heads and tails of our 2FIFO list.  Due to differences in style,
// the 2FIFO lists use 'next' to point towards the tail, and the FIFO
// lists use 'next' to point towards the head.
f_node *ff_head = NULL;
f_node *ff_tail = NULL;
f_node *sf_head = NULL;
f_node *sf_tail = NULL;

void fifo_insert(int frame_index);
int  fifo_remove();

// We use separate functions to handle the second-chance FIFO insertions/removals.
void sfo_insert(struct page_table *pt, f_node * node);
int sfo_remove(f_node * node, f_node ** head);

void evict(struct page_table * pt, int f_num);

/**
 * Generic page fault handler.
 */
void page_fault_handler( struct page_table *pt, int page ) {
    ++stats.page_faults;
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


/**
 * Main function.  Performs basic setup and parses arguments.
 */
int main( int argc, char *argv[] ) {
	if(argc!=5) {
		printf("use: virtmem <npages> <nframes> <rand|fifo|2fifo|custom> <sort|scan|focus>\n");
		return 1;
	}

    memset(&args, 0, sizeof(struct args));
    args.npages  = atoi(argv[1]);
    args.nframes = atoi(argv[2]);
    args.policy  = argv[3];
    args.program = argv[4];

    if (args.npages < 1 || args.nframes < 1) {
        printf("invalid argument: number of pages and frames must be greater than 0\n");
        return 1;
    }
    
    if (args.nframes < 5) {
        FIRST_L = args.nframes - 1;
        SECOND_L = 1;
    }
    else {
        FIRST_L = args.nframes * 3/4;
        SECOND_L = args.nframes * 1/4;
        if (args.nframes % 4 != 0) {
            FIRST_L++;
        }
    }

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
    frame_table = malloc(args.nframes * sizeof(f_node));
    if (frame_table == NULL) {
        printf("Warning: could not allocate space for frame database!\n");
        exit(1);
    }
    memset(frame_table, 0, args.nframes * sizeof(f_node));
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
	
	// Initial value for how many entries we allow to be unmodified before evicting one when we search.
	// Used in the custom algorithm.
	chance = args.nframes/3;

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

	return 0;
}


/**
 * Random handler.
 */
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
            evict(pt, frame_index);
        }
        // Read in from disk to physical memory
        disk_read(disk, page, &physmem[frame_index * PAGE_SIZE]);
        ++stats.disk_reads;
    } else if (bits & PROT_READ && !(bits & PROT_WRITE)) { // Missing write bit
        bits |= PROT_WRITE;
        frame_index = frame;
    } else { // Shouldn't get here...
        printf("Warning: entered page fault handler for page with all protection bits enabled\n");
        return;
    }

    // Update the page table entry for this page
    page_table_set_entry(pt, page, frame_index, bits);
    PAGE(frame_index) = page;
    BITS(frame_index) = bits;

    // Mark the frame as used
    FREE(frame_index) = 1;

}

/**
 * Standard FIFO handler.
 */
void page_fault_handler_fifo( struct page_table *pt, int page ) {
    int frame, bits;
    page_table_get_entry(pt, page, &frame, &bits);

    // Update protection bits and find the frame index for page loading
    int frame_index = -1;
    if (!bits) { // Missing read bit
        bits |= PROT_READ;
        if ((frame_index = find_free_frame()) < 0) {
            // Evict page from tail of queue, eg. fifo_head
            if ((frame_index = fifo_remove()) < 0) {
                printf("Warning: attempted to remove frame index from empty fifo!\n");
                return;
            }
            evict(pt, frame_index);
        }
        // Read in from disk to physical memory
        disk_read(disk, page, &physmem[frame_index * PAGE_SIZE]);
        ++stats.disk_reads;
    } else if (bits & PROT_READ && !(bits & PROT_WRITE)) { // Missing write bit
        bits |= PROT_WRITE;
        frame_index = frame;
    } else { // Shouldn't get here?
        printf("Warning: entered page fault handler for page with all protection bits enabled\n");
        return;
    }

    // Update the page table entry for this page
    page_table_set_entry(pt, page, frame_index, bits);
    PAGE(frame_index) = page;
    BITS(frame_index) = bits;

    // Mark the frame as used and insert it into the fifo
    FREE(frame_index) = 1;
    fifo_insert(frame_index);

}


/**
 * Second-chance FIFO handler.
 */
void page_fault_handler_2fifo( struct page_table *pt, int page ) {
    int frame, bits;
    page_table_get_entry(pt, page, &frame, &bits);

    // Update protection bits and find the frame index for page loading
    int frame_index = -1;
    if (bits == PROT_NONE) { // Missing read and write bit
        bits |= PROT_READ;
        //Get the corresponding frame node
        f_node * tempNode = &frame_table[frame]; 
        if ((page == PAGE(frame)) && tempNode->f_list == 2) { //Note: the page == PAGE(frame) check is to ensure it's paged in.
            //We have an entry in the second chance list, bump that back up
            sfo_remove(tempNode, &sf_head);
            
            // Decrement the number of second-chance entries, insert to the first-chance list, and update the releavnt fields.
            s_entries--;
            sfo_insert(pt, tempNode);
            tempNode->f_list = 1;
            frame_index = frame;
            bits = tempNode->bits;
        }
        else if ((page == PAGE(frame)) && tempNode->f_list == 1) {
            printf("We have a read page fault, yet we're in the first 2FIFO list.  This should be impossible.\n");
            exit(1);
        }
        else {
            // We need a new node.
            frame_index = find_free_frame();
            if (frame_index == -1) {
                // We have no free frames.  We need to evict the oldest page.
                // Evict from second-chance list if present (which it should be except in very low frame cases); otherwise evict from first list.
                if (sf_head == NULL) {
                    frame_index = sfo_remove(ff_head, &ff_head);
                    f_entries--;
                }
                else {
                    frame_index = sfo_remove(sf_head, &sf_head);
                    s_entries--;
                }
                if (frame_index == -1) {
                    printf("2FIFO: could not get a frame by removing.  What?\n");
                    return;
                }
                evict(pt, frame_index);
            }
            // Fetch our evicted node and update its relevant fields.
            tempNode = &frame_table[frame_index];
            tempNode->page = page;
            sfo_insert(pt, tempNode);
            tempNode->f_list = 1;
            // Read in from disk to physical memory
            disk_read(disk, page, &physmem[frame_index * PAGE_SIZE]);
            ++stats.disk_reads;
        }
    } else if (bits & PROT_READ && !(bits & PROT_WRITE)) { // Missing write bit
        bits |= PROT_WRITE;
        frame_index = frame;
    } else { // Shouldn't get here?
        printf("Warning: entered page fault handler for page with all protection bits enabled\n");
        return;
    }
    
    // Update the page table entry for this page
    if (frame_table[frame_index].f_list != 2) {
        page_table_set_entry(pt, page, frame_index, bits);
        PAGE(frame_index) = page;
        BITS(frame_index) = bits;
    }

    // Mark the frame as used
    FREE(frame_index) = 1;

}


/**
 * Handler for our custom algorithm.  First looks for free frames, then 
 * unmodified ones, and finally goes with the oldest entry a la FIFO.
 */
void page_fault_handler_custom( struct page_table *pt, int page ) {
    int frame, bits;
    page_table_get_entry(pt, page, &frame, &bits);

    // Update protection bits and find the frame index for page loading
    int frame_index = -1;
    if (!bits) { // Missing read bit
        bits |= PROT_READ;
        if ((frame_index = find_free_frame()) < 0) {
            // Evict clean page (if there is one)
            if ((frame_index = find_clean_frame()) < 0) {
                // No clean pages available, remove oldest one via FIFO list
                if ((frame_index = fifo_remove()) < 0) {
                    printf("Warning: attempted to remove frame index from empty fifo!\n");
                    return;
                }
            }
            evict(pt, frame_index);
        }
        // Read in from disk to physical memory
        disk_read(disk, page, &physmem[frame_index * PAGE_SIZE]);
        ++stats.disk_reads;
    } else if (bits & PROT_READ && !(bits & PROT_WRITE)) { // Missing write bit
        bits |= PROT_WRITE;
        frame_index = frame;
    } else { // Shouldn't get here?
        printf("Warning: entered page fault handler for page with all protection bits enabled\n");
        return;
    }

    // Update the page table entry for this page
    page_table_set_entry(pt, page, frame_index, bits);
    PAGE(frame_index) = page;
    BITS(frame_index) = bits;

    // Mark the frame as used and insert it into the fifo
    FREE(frame_index) = 1;
    fifo_insert(frame_index);

}


/**
 * Search for unused frame, return its index if found or -1 if none available
 */
int find_free_frame() {
    // Search frame table for a free (unused) frame, return its index if found
    for (int i = 0; i < args.nframes; ++i) {
        if (FREE(i) == 0) return i;
    }
    // No free frames were found, return error code
    return -1;
}


/**
 * Searches for a non-dirty page that can be evicted without writing back to disk.
 * Which page we ultimately return depends on how many unmodified pages we have.
 */
int find_clean_frame() {
    // Search frame table for a clean frame, return its index if found
    f_node * node = fifo_tail->next;
    f_node * candidate = NULL;
    chance = args.nframes * 5/6;
    int i = 0;
    while (node != NULL && i < chance) {
        if (node->bits & (~PROT_WRITE)) {
            i++;
            candidate = node;
        }
        node = node->next;
    }
    if ( candidate != NULL ) {
        node = candidate;
            if (node == fifo_tail) {
                fifo_tail = fifo_tail->next;
                if (fifo_tail != NULL) {
                    fifo_tail->prev = NULL;
                }
                else {
                    fifo_head = fifo_tail;
                }
                node->f_list = 0;
                return FRAMEID(node);
            }
            else {
                if (node->next != NULL) {
                    node->next->prev = node->prev;
                }
                    node->prev->next = node->next;
                    node->f_list = 0;
                    return FRAMEID(node);
            }
         }
    else {
    // No clean pages were found, return error code
        return -1;
    }
    return -1;
}

/**
 * Create and insert a new node with the specified frame_index into the fifo list
 */
void fifo_insert(int frame_index) {

    f_node * node = &frame_table[frame_index];

    // Insert frame_index into fifo at tail (making it the new tail)
    if (fifo_tail == NULL) { // No nodes in list
        fifo_head = node;
        fifo_tail = node;
        node->f_list = 1;
    } else {                 // Nodes in list
        // See if the frame_index is already in the list
        if (node->f_list == 1) {
            // Don't insert already inserted items
            return;
        }

        node->next = fifo_tail;
        node->prev = NULL;
        fifo_tail-> prev = node;
        fifo_tail  = node;
        node->f_list = 1;

    }
}

/**
 * Remove the head node of the fifo list and return its frame_index value,
 * or -1 if the fifo list is empty
 */
int fifo_remove() {

    if (fifo_head == NULL) { // Nothing to remove
        return -1;
    } else if (fifo_head == fifo_tail) { // Only 1 element
        int frame_index = FRAMEID(fifo_head);
        fifo_head->f_list = 0;
        fifo_head = fifo_tail = NULL;
        return frame_index;
    }

    // Remove the head of the fifo
    fifo_head->f_list = 0;
    int frame_index = FRAMEID(fifo_head);
    fifo_head = fifo_head->prev;
    fifo_head->next = NULL;

    return frame_index;
}

/**
 * Insert a node into the combined first- and second-chance lists.
 * In the event that the first list is full, this properly moves one to the second list.
 * If that is full as well, this properly evicts the oldest page of the second list.
 */
void sfo_insert(struct page_table *pt, f_node * node) {
    // Insert node into the first-chance list.
    if (ff_head == NULL) {
        ff_head = node;
        ff_tail = node;
        node->next = NULL;
        node->prev = NULL;
    }
    else {
        ff_tail->next = node;
        node->prev = ff_tail;
        ff_tail = node;
        node->next = NULL;
    }
    node->f_list = 1;
    f_entries++;
    if (f_entries > FIRST_L) {
        // The first-chance list is full; we need to bump one to the second-chance list.
        // Check if the second list is empty.
        if (sf_head == NULL) {
            sf_head = ff_head;
            sf_tail = sf_head;
            ff_head = ff_head->next;
            ff_head->prev = NULL;
            sf_head->next = NULL;
        }
        else {
            node = ff_head;
            sfo_remove(node, &ff_head);
            sf_tail->next = node;
            node->prev = sf_tail;
            node->next = NULL;
            sf_tail = node;
        }
        // Update the associated list of the newly-inserted node, and invalidate the page.
        sf_tail->f_list = 2;
        page_table_set_entry(pt, sf_tail->page, FRAMEID(sf_tail), PROT_NONE);
        
        s_entries++;
        if (s_entries > SECOND_L) {
            // We have too many entries in the second list and must evict a page.
            evict(pt, FRAMEID(sf_head));
            sf_head->free = 0;
            sf_head->f_list = 0;
            sf_head = sf_head->next;
            sf_head->prev = NULL;
            s_entries--;
        }
        f_entries--;
    }
}

/** Remove a node from our first- or second-chance list and returns its frame number.
 * This does _not_ free the node or evict it to disk, since that depends on the context it's called in.
 * Note the double-pointer is so we can use this with either list's head.
 */
int sfo_remove(f_node * node, f_node ** head) {
    //f_node * tempNode = *head;
    if (*head == NULL) {
    printf("Error: We're removing from an empty list!\n");
    exit(1);
    }
    if (node == *head) {
        //special case
        int frame = FRAMEID((*head));
        *head = (*head)->next;
        if ((*head) != NULL)
        {
        (*head)->prev = NULL;
        }
        return frame;
    }
    else {
        if (node == sf_tail)
        {
            //Special case
            sf_tail = sf_tail->prev;
            sf_tail->next = NULL;
        }
        else if (node == ff_tail)
        {
            //Special case
            ff_tail = ff_tail->prev;
            ff_tail->next = NULL;
        }
        else
        {
            node->prev->next = node->next;
            node->next->prev = node->prev;
        }
        return FRAMEID(node);
   }
}

/**
 * Evicts the page that is in the frame indexed by f_num, writing to disk first if needed
 */
void evict(struct page_table * pt, int f_num) {
    //NOTE: We assume that write bit set implies a modification was made.
    
    if (BITS(f_num) & PROT_WRITE) {
        disk_write(disk, PAGE(f_num), &physmem[f_num * PAGE_SIZE]);
        ++stats.disk_writes;
    }
    page_table_set_entry(pt, PAGE(f_num), f_num, PROT_NONE);
    BITS(f_num) = PROT_NONE;
    ++stats.evictions;
}

/**
 * Prints some statistics in a slightly understandable manner.
 */
void print_stats() {
    printf("\nStatistics:  flt(%d) rd(%d) wr(%d) ev(%d)\n",
        stats.page_faults, stats.disk_reads, stats.disk_writes, stats.evictions);
}

/**
 * Prints some statistics in a totally non-understandable manner, unless you're Excel.
 */
void graph_stats() {
    //NUMPAGES NUMFRAMES FAULTS READS WRITES EVICTIONS
    printf("%i %i %i %i %i %i\n", args.npages, args.nframes, stats.page_faults, stats.disk_reads, stats.disk_writes, stats.evictions);
}

