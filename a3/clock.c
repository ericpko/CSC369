#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <getopt.h>
#include <stdlib.h>
#include "pagetable.h"


extern int memsize;

extern int debug;

extern struct frame *coremap;

/* Page to evict is chosen using the clock algorithm.
 * Returns the page frame number (which is also the index in the coremap)
 * for the page that is to be evicted.
 *
 * Clock Algorithm as described by the textbook: When it encounters
 * a page with a reference bit set to 1, it clears the bit (i.e., sets it
 * to 0); when it finds a page with the reference bit set to 0, it chooses it as
 * its victim.
 */

int clock_evict() {

    int pfn;
    for (;;) {
        pfn = (int)(random() % memsize);            // try a random page
        if (coremap[pfn].pte->frame & PG_REF) {
            // Ref bit is set to 1, so set its REF bit to 0 and try again
            coremap[pfn].pte->frame &= ~PG_REF;
        } else {
            // Ref bit is set to 0. Victim found!
            return pfn;
        }
    }

    // Shouldn't get here
    fprintf(stderr, "clock_evict error");
	return -1;
}


/* This function is called on each access to a page to update any information
 * needed by the clock algorithm.
 * Input: The page table entry for the page that is being accessed.
 */
void clock_ref(pgtbl_entry_t *p) {

    p->frame |= PG_REF;
}

/* Initialize any data structures needed for this replacement
 * algorithm.
 */
void clock_init() {
    // Nothing to initialize
}





/*
 * Textbook suggestion to prioritize non-dirty pages. Performance is slightly
 * worse than the version without proitizing non-dirty pages.
 */

// int clock_evict() {

//     int pfn;
//     for (;;) {
//         pfn = (int)(random() % memsize);
//         if (coremap[pfn].pte->frame & PG_REF) {
//             // Ref bit is set to 1, so set its REF bit to 0 and try again
//             coremap[pfn].pte->frame &= ~PG_REF;
//         } else {

//             if (coremap[pfn].pte->frame & PG_DIRTY) {
//                 // Prioritize choosing nondirty pages for better performance.
//                 if (max_attempts == memsize - 1) {
//                     // We'll have to just pick a dirty page.
//                     return pfn;
//                 }
//             } else {
//                 // Ref bit is 0 AND dirty bit is set to 0. Victim found!
//                 return pfn;
//             }
//             // Ref bit is set to 0. Victim found!
//             // return pfn;
//         }
//         max_attempts = (max_attempts + 1) % memsize;
//     }

//     // Shouldn't get here
//     fprintf(stderr, "clock_evict error");
// 	return -1;
// }





// Working version
// int clock_evict() {

//     int pfn;
//     for (;;) {
//         pfn = (int)(random() % memsize);            // try a random page
//         if (coremap[pfn].pte->frame & PG_REF) {
//             // Ref bit is set to 1, so set its REF bit to 0 and try again
//             coremap[pfn].pte->frame &= ~PG_REF;
//         } else {
//             // Ref bit is set to 0. Victim found!
//             return pfn;
//         }
//     }

//     // Shouldn't get here
//     fprintf(stderr, "clock_evict error");
// 	return -1;
// }
