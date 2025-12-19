// memstat.h - Memory statistics structures for demand paging system

#ifndef _MEMSTAT_H_
#define _MEMSTAT_H_

#include "types.h"

#define MAX_PAGES_INFO 128 // Max pages to report per syscall

// Page states
#define UNMAPPED 0 
#define RESIDENT 1 
#define SWAPPED  2

struct page_stat {
  uint va;       // Virtual address of the page (page-aligned)
  int state;     // Page state: UNMAPPED, RESIDENT, or SWAPPED
  int is_dirty;  // 1 if page has been written to, 0 otherwise
  int seq;       // FIFO sequence number (for resident pages)
  int swap_slot; // Swap slot number (for swapped pages, -1 otherwise)
};

struct proc_mem_stat {
  int pid;                                    // Process ID
  int num_pages_total;                        // Total number of virtual pages
  int num_resident_pages;                     // Number of pages currently in physical memory
  int num_swapped_pages;                      // Number of pages currently swapped out
  int next_fifo_seq;                          // Next FIFO sequence number to be assigned
  struct page_stat pages[MAX_PAGES_INFO];    // Array of page information
};

#endif // _MEMSTAT_H_
