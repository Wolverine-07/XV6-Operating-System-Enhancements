#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/memstat.h"

int
main(int argc, char *argv[])
{
  struct proc_mem_stat info;
  
  printf("Testing memstat system call:\n");
  
  if(memstat(&info) < 0) {
    printf("memstat failed\n");
    exit(1);
  }
  
  printf("PID: %d\n", info.pid);
  printf("Total pages: %d\n", info.num_pages_total);
  printf("Resident pages: %d\n", info.num_resident_pages);
  printf("Swapped pages: %d\n", info.num_swapped_pages);
  printf("Next FIFO seq: %d\n", info.next_fifo_seq);
  
  printf("First few pages:\n");
  for(int i = 0; i < 5 && i < MAX_PAGES_INFO; i++) {
    if(info.pages[i].va > 0 || info.pages[i].state != UNMAPPED) {
      printf("  va=0x%x state=%d dirty=%d seq=%d slot=%d\n",
             info.pages[i].va, info.pages[i].state, 
             info.pages[i].is_dirty, info.pages[i].seq, 
             info.pages[i].swap_slot);
    }
  }
  
  // Test malloc to create heap allocation
  char *ptr = malloc(4096);
  if(ptr) {
    *ptr = 'A';  // Make it dirty
    printf("Allocated and accessed heap page\n");
    
    if(memstat(&info) < 0) {
      printf("second memstat failed\n");
      exit(1);
    }
    printf("After malloc - Resident: %d\n", info.num_resident_pages);
    free(ptr);
  }
  
  exit(0);
}
