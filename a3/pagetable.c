#include <assert.h>
#include <string.h>
#include "sim.h"
#include "pagetable.h"

// The top-level page table (also known as the 'page directory')
pgdir_entry_t pgdir[PTRS_PER_PGDIR];

// Counters for various events.
// Your code must increment these when the related events occur.
int hit_count = 0;
int miss_count = 0;
int ref_count = 0;
int evict_clean_count = 0;
int evict_dirty_count = 0;

/*
 * Allocates a frame to be used for the virtual page represented by p.
 * If all frames are in use, calls the replacement algorithm's evict_fcn to
 * select a victim frame.  Writes victim to swap if needed, and updates
 * pagetable entry for victim to indicate that virtual page is no longer in
 * (simulated) physical memory.
 *
 * Counters for evictions should be updated appropriately in this function.
 */
int allocate_frame(pgtbl_entry_t *p) {
	int i;
	int frame = -1;
	for(i = 0; i < memsize; i++) {
		if(!coremap[i].in_use) {
			frame = i;
			break;
		}
	}
	if(frame == -1) { // Didn't find a free page.
		// Call replacement algorithm's evict function to select victim
		frame = evict_fcn();    // returns a 32 bit unsigned int PFN

		// All frames were in use, so victim frame must hold some page
		// Write victim page to swap, if needed, and update pagetable
		// IMPLEMENTATION NEEDED

        // Get the victim PTE from the evicted frame
        pgtbl_entry_t *pte = coremap[frame].pte;

        // Check if the dirty bit has been set to 1 (i.e. page has been modified)
        if (pte->frame & PG_DIRTY) {
            int swap_offset;
            if ((swap_offset = swap_pageout(frame, pte->swap_off)) == INVALID_SWAP) {
                fprintf(stderr, "allocate_frame INVALID_SWAP");
                exit(EXIT_FAILURE);
            }
            // Set the victim PTE's swap_off, and set the ONSWAP bit to 1
            pte->swap_off = swap_offset;
            pte->frame |= PG_ONSWAP;        // PG_ONSWAP 1000

            // Update counter
            evict_dirty_count++;
        } else {
            // Update counter
            evict_clean_count++;
        }

        // No longer dirty because it's being stored
        pte->frame &= ~PG_DIRTY;
        // Set the victim PTE's frame to be invalid (i.e. ~PG_VALID = 11..0)
        // since this physical frame is no longer pointing to this PTE
        pte->frame &= ~PG_VALID;    // sets the lowest-order bit to 0
	}

	// Record information for virtual page that will now be stored in frame
	coremap[frame].in_use = 1;
	coremap[frame].pte = p;

	return frame;
}

/*
 * Initializes the top-level pagetable.
 * This function is called once at the start of the simulation.
 * For the simulation, there is a single "process" whose reference trace is
 * being simulated, so there is just one top-level page table (page directory).
 * To keep things simple, we use a global array of 'page directory entries'.
 *
 * In a real OS, each process would have its own page directory, which would
 * need to be allocated and initialized as part of process creation.
 */
void init_pagetable() {
	int i;
	// Set all entries in top-level pagetable to 0, which ensures valid
	// bits are all 0 initially.
	for (i=0; i < PTRS_PER_PGDIR; i++) {
		pgdir[i].pde = 0;
	}
}

// For simulation, we get second-level pagetables from ordinary memory
pgdir_entry_t init_second_level() {
	int i;
	pgdir_entry_t new_entry;
	pgtbl_entry_t *pgtbl;

	// Allocating aligned memory ensures the low bits in the pointer must
	// be zero, so we can use them to store our status bits, like PG_VALID
	if (posix_memalign((void **)&pgtbl, PAGE_SIZE,
			   PTRS_PER_PGTBL*sizeof(pgtbl_entry_t)) != 0) {
		perror("Failed to allocate aligned memory for page table");
		exit(1);
	}

	// Initialize all entries in second-level pagetable
	for (i=0; i < PTRS_PER_PGTBL; i++) {
		pgtbl[i].frame = 0; // sets all bits, including valid, to zero
		pgtbl[i].swap_off = INVALID_SWAP;
	}

	// Mark the new page directory entry as valid
	new_entry.pde = (uintptr_t)pgtbl | PG_VALID;

	return new_entry;
}

/*
 * Initializes the content of a (simulated) physical memory frame when it
 * is first allocated for some virtual address.  Just like in a real OS,
 * we fill the frame with zero's to prevent leaking information across
 * pages.
 *
 * In our simulation, we also store the the virtual address itself in the
 * page frame to help with error checking.
 *
 */
void init_frame(int frame, addr_t vaddr) {
	// Calculate pointer to start of frame in (simulated) physical memory
	char *mem_ptr = &physmem[frame*SIMPAGESIZE];
	// Calculate pointer to location in page where we keep the vaddr
    addr_t *vaddr_ptr = (addr_t *)(mem_ptr + sizeof(int));

	memset(mem_ptr, 0, SIMPAGESIZE); // zero-fill the frame
	*vaddr_ptr = vaddr;             // record the vaddr for error checking

	return;
}

/*
 * Locate the physical frame number for the given vaddr using the page table.
 *
 * If the entry is invalid and not on swap, then this is the first reference
 * to the page and a (simulated) physical frame should be allocated and
 * initialized (using init_frame).
 *
 * If the entry is invalid and on swap, then a (simulated) physical frame
 * should be allocated and filled by reading the page data from swap.
 *
 * Counters for hit, miss and reference events should be incremented in
 * this function.
 */
char *find_physpage(addr_t vaddr, char type) {

    /* Note: Assume vaddr is 36 bits. Offset is 12 bits. 24 bits VPN.
     * PGDIR_SHIFT shifts 24 bits, so 36 - 24 = top 12 bits of vaddr
     * Thus the 24 bit VPN has top 12 bits for PDIndex and lower 12
     * bits for PTIndex.
     */

    // Get the page directory index from the vaddr
	unsigned pd_idx = PGDIR_INDEX(vaddr); // top 12 bits of vaddr

    // Check if the 2nd-level page table is invalid, if so, initialize
    if ((pgdir[pd_idx].pde & PG_VALID) == 0) {
        pgdir[pd_idx] = init_second_level();
    }

	// Use vaddr to get index into 2nd-level page table and initialize 'pte'
    /* Note: pgdir[pd_inx].pde "is a pointer to a page table" and
     * "the page tables are arrays of page table entries"
     */
    pgtbl_entry_t *pgtbl = (pgtbl_entry_t *)(pgdir[pd_idx].pde & PAGE_MASK); // lower 12 bits are 0
    pgtbl_entry_t *pte = &pgtbl[PGTBL_INDEX(vaddr)];
    // pgtbl_entry_t *pte = PGTBL_INDEX(vaddr) + pgtbl; // equivalent to above


	// Check if pte is valid or not, on swap or not, and handle appropriately
    if ((pte->frame & PG_VALID) == 0) {
        // PTE is invalid (physical frame is not holding vpage)
        int frame = allocate_frame(pte);    // only the PFN (no status bits)

        // Check if the PTE is not on swap
        if ((pte->frame & PG_ONSWAP) == 0) {
            // then we need to initialize the new frame
            init_frame(frame, vaddr);
            pte->frame = (frame << PAGE_SHIFT);

            // Set dirty bit on invalid and not on swap no matter the type
            pte->frame |= PG_DIRTY;

            // Frame should now be valid, dirty, referenced, not on swap

        } else {
            // then the PTE is on swap, so swap in the page
            if ((swap_pagein(frame, pte->swap_off)) != 0) {
                perror("swap_pagein");
                exit(EXIT_FAILURE);
            }
            // New frame so all status bits are zero
            pte->frame = (frame << PAGE_SHIFT);

            // Frame should now be valid, not dirty, referenced, not on swap
        }

        miss_count++;

    } else {
        // The physical frame is holding this vpage
        hit_count++;
    }

	// Make sure that pte is marked valid and referenced. Also mark it
	// dirty if the access type indicates that the page will be written to.
    // Note: Storing 'S' we have to write
    if (type == 'M' || type == 'S') {
        pte->frame |= PG_DIRTY;
    }
    pte->frame |= PG_VALID;
    pte->frame |= PG_REF;
    pte->frame &= ~PG_ONSWAP;   // Would make no sense to be on swap

	// Call replacement algorithm's ref_fcn for this page
	ref_fcn(pte);
    ref_count++;

	// Return pointer into (simulated) physical memory at start of frame
	return &physmem[(pte->frame >> PAGE_SHIFT) * SIMPAGESIZE];
}

void print_pagetbl(pgtbl_entry_t *pgtbl) {
	int i;
	int first_invalid, last_invalid;
	first_invalid = last_invalid = -1;

	for (i=0; i < PTRS_PER_PGTBL; i++) {
		if (!(pgtbl[i].frame & PG_VALID) &&
		    !(pgtbl[i].frame & PG_ONSWAP)) {
			if (first_invalid == -1) {
				first_invalid = i;
			}
			last_invalid = i;
		} else {
			if (first_invalid != -1) {
				printf("\t[%d] - [%d]: INVALID\n",
				       first_invalid, last_invalid);
				first_invalid = last_invalid = -1;
			}
			printf("\t[%d]: ",i);
			if (pgtbl[i].frame & PG_VALID) {
				printf("VALID, ");
				if (pgtbl[i].frame & PG_DIRTY) {
					printf("DIRTY, ");
				}
				printf("in frame %d\n",pgtbl[i].frame >> PAGE_SHIFT);
			} else {
				assert(pgtbl[i].frame & PG_ONSWAP);
				printf("ONSWAP, at offset %lu\n",
				       (unsigned long)pgtbl[i].swap_off);
			}
		}
	}
	if (first_invalid != -1) {
		printf("\t[%d] - [%d]: INVALID\n", first_invalid, last_invalid);
		first_invalid = last_invalid = -1;
	}
}

void print_pagedirectory() {
	int i; // index into pgdir
	int first_invalid,last_invalid;
	first_invalid = last_invalid = -1;

	pgtbl_entry_t *pgtbl;

	for (i=0; i < PTRS_PER_PGDIR; i++) {
		if (!(pgdir[i].pde & PG_VALID)) {
			if (first_invalid == -1) {
				first_invalid = i;
			}
			last_invalid = i;
		} else {
			if (first_invalid != -1) {
				printf("[%d]: INVALID\n  to\n[%d]: INVALID\n",
				       first_invalid, last_invalid);
				first_invalid = last_invalid = -1;
			}
			pgtbl = (pgtbl_entry_t *)(pgdir[i].pde & PAGE_MASK);
			printf("[%d]: %p\n",i, pgtbl);
			print_pagetbl(pgtbl);
		}
	}
}
