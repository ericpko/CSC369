#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <getopt.h>
#include <stdlib.h>
#include "pagetable.h"


extern int memsize;

extern int debug;

extern struct frame *coremap;

typedef struct __node_t {
    unsigned frame;
    struct __node_t *next;
    struct __node_t *prev;
} node_t;


typedef struct __list_t {
    unsigned size;
    node_t *head;               // LRU
    node_t *tail;               // MRU
} list_t;


list_t list;                    // A linked list
node_t **map;                   // An array of pointers to nodes




/* Page to evict is chosen using the accurate LRU algorithm.
 * Returns the page frame number (which is also the index in the coremap)
 * for the page that is to be evicted.
 *
 *
 * Keep a time stamp in each PTE, updated on each reference and
 * scan all the PTEs when choosing a victim to find the PTE with
 * the oldest timestamp.
 */
int lru_evict() {

    // Get a pointer to the node we are removing from the list
    node_t *victim = list.head;

    // Either the list contains a single node, or more than one node
    // Case 1: The list contains a single node
    if (list.head == list.tail) {
        list.head = list.tail = NULL;

    // Case 2: The list contains at least two nodes
    } else {
        list.head = list.head->next;
        list.head->prev = NULL;
    }

    // Get the frame to evict
    int pfn = victim->frame;
    map[pfn] = NULL;
    free(victim);
    list.size--;

    return pfn;
}


/* This function is called on each access to a page to update any information
 * needed by the lru algorithm.
 * Input: The page table entry for the page that is being accessed.
 */
void lru_ref(pgtbl_entry_t *p) {

    // Save the PFN from the incoming PTE
    unsigned frame = (p->frame >> PAGE_SHIFT);

    // Get a pointer to the target node, if it's in there
    node_t *targ = map[frame];

    if (targ == NULL) {
        /*
         * This is the first time we're referencing this frame, so create
         * a new node and attach it to the back of the list (the MRU side).
         */
        node_t *node = malloc(sizeof(node_t));
        node->frame = frame;
        node->next = node->prev = NULL;

        // Case 1: The list is empty
        if (list.size == 0) {
            list.head = node;
            list.tail = node;

        // Case 2: There is at least one node in the list. We add to the tail (MRU)
        } else {
            list.tail->next = node;
            node->prev = list.tail;
            list.tail = node;
        }

        // Store a point to this node
        map[frame] = node;

        list.size++;


    } else {
        /*
         * This frame is being referenced again, so put this node to the
         * tail of the linked list (the MRU side).
         */

        // Case 1: The list contains a single node
        if (list.head == list.tail) {
            // Nothing to update
            ;

        // Case 2: The list contains at least two node
        } else {
            // Case A: Target node is at the head
            if (targ == list.head) {
                list.head = list.head->next;
                list.head->prev = NULL;
                list.tail->next = targ;
                targ->prev = list.tail;
                targ->next = NULL;
                list.tail = targ;

            // Case B: Target node is at the tail
            } else if (targ == list.tail) {
                // Do nothing because the tail is the MRU side
                ;

            // Case C: There are at least three nodes and targ is not head or tail
            } else {
                // Unlink the target node from the list
                targ->prev->next = targ->next;
                targ->next->prev = targ->prev;

                // Move targ to the MRU tail
                targ->prev = list.tail;
                targ->next = NULL;
                list.tail->next = targ;
                list.tail = targ;
            }
        }
    }
}


/* Initialize any data structures needed for this
 * replacement algorithm
 */
void lru_init() {

    list.size = 0;
    list.head = NULL;
    list.tail = NULL;

    map = malloc(memsize * sizeof(node_t *));
    for (int i = 0; i < memsize; i++) {
        map[i] = NULL;
    }

}































/*
 * A succinct version of lru.c, but O(M) on evic, O(1) else
 */
// #include <stdio.h>
// #include <assert.h>
// #include <unistd.h>
// #include <getopt.h>
// #include <stdlib.h>
// #include "pagetable.h"


// extern int memsize;

// extern int debug;

// extern struct frame *coremap;

// int curr_time;

// /* Page to evict is chosen using the accurate LRU algorithm.
//  * Returns the page frame number (which is also the index in the coremap)
//  * for the page that is to be evicted.
//  *
//  *
//  * Keep a time stamp in each PTE, updated on each reference and
//  * scan all the PTEs when choosing a victim to find the PTE with
//  * the oldest timestamp.
//  */

// int lru_evict() {

//     int pfn = 0;
//     int least_recently_used = curr_time;

//     // Find the frame that has the least recently updated timestamp
//     for (int i = 0; i < memsize; i++) {
//         if (coremap[i].timestamp < least_recently_used) {
//             least_recently_used = coremap[i].timestamp;
//             pfn = i;
//         }
//     }

// 	return pfn;
// }

// /* This function is called on each access to a page to update any information
//  * needed by the lru algorithm.
//  * Input: The page table entry for the page that is being accessed.
//  */
// void lru_ref(pgtbl_entry_t *p) {

//     // This frame was just accessed so update it's timestamp back to the top
//     // Note the ++ increments the value of curr_time AFTER assignment
//     coremap[p->frame >> PAGE_SHIFT].timestamp = curr_time++;
// }


// /* Initialize any data structures needed for this
//  * replacement algorithm
//  */
// void lru_init() {

//     // All timestamps when frame was last referenced set to zero
//     curr_time = 0;
// }
