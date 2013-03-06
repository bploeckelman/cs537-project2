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
#define FIRST_L 200
#define SECOND_L 50 //Sizes for first and second-chance lists
#define PAGE(x) frame_table[x].page
#define BITS(x) frame_table[x].bits
#define FREE(x) frame_table[x].free
#define FRAMEID(x) ((int) (x - frame_table))

struct disk *disk = NULL;
char *virtmem = NULL;
char *physmem = NULL;
int f_entries = 0;
int s_entries = 0;


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
int find_clean_frame();


// FIFO list ------------------------------------------------------------------
typedef struct _f_node {
    int page;
    int bits;
    int free;
    int f_list; //0 if in none, 1 if in FIFO or first-chance, 2 if in second
    struct _f_node * next;
} f_node;

// Our page frame database.  Each entry is an above-defined f_node.
f_node *frame_table = NULL;

f_node *fifo_head = NULL;
f_node *fifo_tail = NULL;

f_node *ff_head = NULL;
f_node *ff_tail = NULL;
f_node *sf_head = NULL;
f_node *sf_tail = NULL;

void fifo_insert(int frame_index);
int  fifo_remove();
void fifo_print();

void sfo_insert(struct page_table *pt, f_node * node);
int sfo_remove(f_node * node, f_node ** head);

void evict(struct page_table * pt, int f_num);

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

    if (args.npages < 1 || args.nframes < 1) {
        printf("invalid argument: number of pages and frames must be greater than 0\n");
        return 1;
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
    if (frame_table == NULL)
    {
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

#ifdef DEBUG
    // Results
    print_stats(&stats);
#endif

	return 0;
}


// ----------------------------------------------------------------------------
void page_fault_handler_rand( struct page_table *pt, int page ) {
    int frame, bits;
    page_table_get_entry(pt, page, &frame, &bits);
    #ifdef DEBUG
        printf("Rand handler: page = %i, frame = %i, bits = %i\n", page, frame, bits);
    #endif

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

#ifdef DEBUG
    printf("Set page %d to frame %d\n", page, frame_index);
    page_table_print_entry(pt, page);
    puts("");
#endif
}

//TODO: Does x86 allow us to have memory set to 'write-only?'

// ----------------------------------------------------------------------------
void page_fault_handler_2fifo( struct page_table *pt, int page ) {
    int frame, bits;
    page_table_get_entry(pt, page, &frame, &bits);

    // Update protection bits and find the frame index for page loading
    int frame_index = -1;
    if (!(bits & PROT_READ)) { // Missing read bit
        bits |= PROT_READ;
        //Get the corresponding frame node
        f_node * tempNode = &frame_table[frame]; 
        if ((page == PAGE(frame)) && tempNode->f_list == 2) { //Note: the page == PAGE(frame) check is to ensure it's paged in.
            //We have an entry in the second chance list, bump that back up
            sfo_remove(tempNode, &sf_head);
            sfo_insert(pt, tempNode);
            tempNode->f_list = 1;
            //TODO: Is the below line necessary?
            //page_table_set_entry(pt, tempNode->page_number, tempNode->frame_index, PROT_READ);
            frame_index = frame;
        }
        else {
            // We need a new node.
            frame_index = find_free_frame();
            if (frame_index == -1) {
                // We have no free frames.  We need to evict the oldest page.
                #ifdef DEBUG
                    printf("2FIFO: Attempting to evict page.  First-chance head: %p, second-chance head: %p\n", ff_head, sf_head);
                #endif
                if (sf_head == NULL)
                {
                    tempNode = ff_head;
                    frame_index = sfo_remove(ff_head, &ff_head);
                    f_entries--;
                }
                else
                {
                    tempNode = sf_head;
                    frame_index = sfo_remove(sf_head, &sf_head);
                    s_entries--;
                }
                if (frame_index == -1)
                {
                    printf("2FIFO: could not get a frame by removing.  What?\n");
                    return;
                }
                evict(pt, frame_index);
                #ifdef DEBUG
                    printf("2FIFO: Evicted page that was in frame %i.\n", frame_index);
                #endif
            }
            tempNode = &frame_table[frame_index];
            tempNode->free = 0;
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
    page_table_set_entry(pt, page, frame_index, bits);
    PAGE(frame_index) = page;
    BITS(frame_index) = bits;

    // Mark the frame as used and insert it into the fifo
    FREE(frame_index) = 1;

#ifdef DEBUG
    printf("Set page %d to frame %d\n", page, frame_index);
    page_table_print_entry(pt, page);
    puts("");
#endif
}


// ----------------------------------------------------------------------------
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
                // No clean pages available, remove one from fifo to evict
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

#ifdef DEBUG
    printf("Set page %d to frame %d\n", page, frame_index);
    page_table_print_entry(pt, page);
    puts("");
#endif
}


/**
 * Search for unused frame, return its index if found or -1 if none available
 **/
int find_free_frame() {
    // Search frame table for a free (unused) frame, return its index if found
    for (int i = 0; i < args.nframes; ++i) {
        if (FREE(i) == 0) return i;
    }
    // No free frames were found, return error code
    return -1;
}

/**
 * Searches for a non-dirty page that can be evicted without writing back to disk
 * @param 
 **/
int find_clean_frame() {
    // Search frame table for a clean frame, return its index if found
    for (int i = 0; i < args.nframes; ++i) {
        if (BITS(i) & (~PROT_WRITE)) return i;
    }
    // No clean pages were found, return error code
    return -1;
}

/**
 * Create and insert a new node with the specified frame_index into the fifo list
 **/
void fifo_insert(int frame_index) {
#ifdef DEBUG
    printf("fifo_insert(): inserting frame #%d", frame_index);
    fifo_print();
#endif

    f_node * node = &frame_table[frame_index];

    // Insert frame_index into fifo at tail (making it the new tail)
    if (fifo_tail == NULL) { // No nodes in list
        fifo_head = node;
        fifo_tail = node;
        node->f_list = 1;
#ifdef DEBUG
        printf("fifo_insert(): inserted\t\t");
        fifo_print();
#endif
    } else {                 // Nodes in list
        // See if the frame_index is already in the list
        if (node->f_list == 1)
        {
            // Don't insert already inserted items
#ifdef DEBUG
            printf("Node for frame #%d already in list, skipping insertion\n", frame_index);
#endif
            return;
        }

        node->next = fifo_tail;
        fifo_tail  = node;
        node->f_list = 1;

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
        int frame_index = FRAMEID(fifo_head);
#ifdef DEBUG
        printf("fifo_remove(): head == tail, 1 element, removing frame #%i", FRAMEID(fifo_head));
#endif
        fifo_head->f_list = 0;
        fifo_head = fifo_tail = NULL;
        fifo_print();
        return frame_index;
    }

    // Remove the head of the fifo
    f_node *node = fifo_tail;
    //TODO: Add a prev pointer to avoid walking the list?
    while (node != NULL && node->next != fifo_head) {
        node = node->next;
    }
    fifo_head->f_list = 0;
    int frame_index = FRAMEID(fifo_head);
    fifo_head = node;
    fifo_head->next = NULL;

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
    f_node *node = fifo_tail;
    while (node != NULL) {
        printf("%i ", FRAMEID(node));
        node = node->next;
    }
    printf("]\n");
}

/**
 * Insert a node into the combined first- and second-chance lists.
 * In the event that the first list is full, this properly moves one to the second list.
 * If that is full as well, this properly evicts the oldest page of the second list.
 **/
void sfo_insert(struct page_table *pt, f_node * node) {
    if (ff_head == NULL)
    {
        ff_head = node;
        ff_tail = node;
        node->next = NULL;
    }
    else
    {
        ff_tail->next = node;
        ff_tail = node;
        node->next = NULL;
    }
    node->f_list = 1;
    f_entries++;
    if (f_entries > FIRST_L)
    {
        //time to move to the second list
        if (sf_head == NULL)
        {
            sf_head = ff_head;
            sf_tail = sf_head;
            ff_head = ff_head->next;
            sf_head->next = NULL;
        }
        else
        {
            node = ff_head;
            sf_tail->next = node;
            sf_tail = node;
            ff_head = ff_head->next;
            node->next = NULL;
        }
        sf_tail->f_list = 2;
        sf_tail->bits = sf_tail->bits & (~PROT_READ);
        page_table_set_entry(pt, sf_tail->page, FRAMEID(sf_tail), sf_tail->bits);
        
        s_entries++;
        if (s_entries > SECOND_L)
        {
            #ifdef DEBUG
                printf("Removing node %p from second list, and evicting page %i in frame %i\n", sf_head, sf_head->page, FRAMEID(sf_head));
            #endif
            //time to evict a page
            evict(pt, FRAMEID(sf_head));
            sf_head->free = 0;
            sf_head->f_list = 0;
            sf_head = sf_head->next;
            s_entries--;
        }
        f_entries--;
    }
}

/** Remove a node from our first- or second-chance list and returns its frame number.
 * This does _not_ free the node or evict it to disk.
 * Note the double-pointer is so we can use this with either list's head.
 */
int sfo_remove(f_node * node, f_node ** head) {
    f_node * tempNode = *head;
    if (node == *head) {
        //special case
        int frame = FRAMEID((*head));
        *head = (*head)->next;
        return frame;
    }
    else {
        while (tempNode != NULL && tempNode->next != node) {
        #ifdef DEBUG
            printf("Remove from second-chance list: current node %p, next node %p\n", tempNode, tempNode->next);
        #endif
            tempNode = tempNode->next;
        }
        tempNode->next = tempNode->next->next;
        return FRAMEID(node);
    }
}

/**
 * Evicts the page that is in the frame indexed by f_num, writing to disk first if needed
 **/
void evict(struct page_table * pt, int f_num)
{
    //NOTE: For now we assume that write bit set implies a difference was made.
#ifdef DEBUG
    printf("Evicting page %i in frame %i, f_num = %i\n", PAGE(f_num), f_num, f_num);
    page_table_print_entry(pt, PAGE(f_num));
    puts("");
#endif
    
    if (BITS(f_num) & PROT_WRITE)
    {
#ifdef DEBUG
        printf("Writing to disk: page number %i, frame number %i\n", PAGE(f_num), f_num);
#endif
        disk_write(disk, PAGE(f_num), &physmem[f_num * PAGE_SIZE]);
        ++stats.disk_writes;
    }
    page_table_set_entry(pt, PAGE(f_num), f_num, 0x0);
    BITS(f_num) = 0x0;
    ++stats.evictions;
}

void print_stats() {
    printf("\nStatistics:  flt(%d) rd(%d) wr(%d) ev(%d)\n",
        stats.page_faults, stats.disk_reads, stats.disk_writes, stats.evictions);
}

