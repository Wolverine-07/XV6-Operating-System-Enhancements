#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "lazyalloc.h"
#include "memstat.h"
#include "fs.h"
#include "file.h"
#include "stat.h"

#define RESIDENT 1
#define SWAPPED 2
#define UNMAPPED 0

void
lazy_init(struct proc *p)
{
  // Initialize demand paging fields
  p->text_start = 0;
  p->text_end = 0;
  p->data_start = 0;
  p->data_end = 0;
  p->heap_start = 0;
  p->stack_top = 0;
  p->next_fifo_seq = 0;
  p->swapfile_inode = 0;
  p->num_swapped_pages = 0;
  p->num_pages = 0;
  p->exec_inode = 0;
  
  for(int i = 0; i < 32; i++) {
    p->swap_slot_bitmap[i] = 0;
  }
  
  for(int i = 0; i < MAX_PROC_PAGES; i++) {
    p->pages[i].va = 0;
    p->pages[i].state = UNMAPPED;
    p->pages[i].is_dirty = 0;
    p->pages[i].seq = 0;
    p->pages[i].swap_slot = -1;
  }
}

void
lazy_alloc_mem(struct proc *p)
{
  // No-op since we're using inline proc fields
  (void)p;
}

void
lazy_free(struct proc *p)
{
  // Clean up swap file
  if(p->swapfile_inode){
    begin_op();
    iput(p->swapfile_inode);
    end_op();
    p->swapfile_inode = 0;
  }
  
  // Clean up exec inode
  if(p->exec_inode){
    begin_op();
    iput(p->exec_inode);
    end_op();
    p->exec_inode = 0;
  }
  
  // Log cleanup
  if(p->num_swapped_pages > 0) {
    printf("[pid %d] SWAPCLEANUP freed_slots=%d\n", p->pid, p->num_swapped_pages);
  }
}

struct page_info*
get_page_info(struct proc *p, uint64 va)
{
  int page_num = (va / PGSIZE) % MAX_PROC_PAGES;
  if(page_num < 0 || page_num >= MAX_PROC_PAGES)
    return 0;
  return &p->pages[page_num];
}

int
lazy_handle_fault(struct proc *p, uint64 va, int write_fault)
{
  va = PGROUNDDOWN(va);
  const char *access_type = write_fault ? "write" : "read";
  
  // If page is already mapped, let vmfault handle it
  if(ismapped(p->pagetable, va)) {
    return -1;
  }
  
  // Check if page is swapped - need to restore it
  struct page_info *pi_swap = get_page_info(p, va);
  if(pi_swap && pi_swap->state == SWAPPED) {
    printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=swap\n", p->pid, va, access_type);
    
    // Allocate physical page for restoration
    uint64 mem = (uint64)kalloc();
    if(mem == 0) {
      // Try to evict a page to make room
      if(lazy_evict_page(p) > 0) {
        mem = (uint64)kalloc();
      }
      
      if(mem == 0) {
        printf("[pid %d] MEMFULL\n", p->pid);
        setkilled(p);
        return -1;
      }
    }
    
    memset((void *)mem, 0, PGSIZE);
    
    // Restore from swap
    if(p->swapfile_inode && pi_swap->swap_slot >= 0) {
      ilock(p->swapfile_inode);
      readi(p->swapfile_inode, 0, mem, (uint64)pi_swap->swap_slot * PGSIZE, PGSIZE);
      iunlock(p->swapfile_inode);
      
      printf("[pid %d] SWAPIN va=0x%lx slot=%d\n", p->pid, va, pi_swap->swap_slot);
    }
    
    // Set permissions based on segment type
    int pte_flags = PTE_U | PTE_R;
    if(va >= p->text_start && va < p->text_end) {
      // Text: executable, read-only
      pte_flags |= PTE_X;
    } else {
      // Data/heap/stack: read-write
      pte_flags |= PTE_W;
    }
    
    if(mappages(p->pagetable, va, PGSIZE, mem, pte_flags) != 0) {
      kfree((void *)mem);
      setkilled(p);
      return -1;
    }
    
    // Update page state
    pi_swap->state = RESIDENT;
    pi_swap->seq = p->next_fifo_seq++;
    int old_slot = pi_swap->swap_slot;
    pi_swap->swap_slot = -1;
    
    // Free the swap slot
    if(old_slot >= 0) {
      free_swap_slot(p, old_slot);
      p->num_swapped_pages--;
    }
    
    printf("[pid %d] RESIDENT va=0x%lx seq=%d\n", p->pid, va, pi_swap->seq);
    
    return 0;
  }
  
  printf("[pid %d] PAGEFAULT va=0x%lx access=%s epc=0x%lx cause=", p->pid, va, access_type, p->trapframe->epc);
  
  int valid = 0;
  const char *cause = "unknown";
  
  // Check if access is valid
  // Stack region is between p->sz - (USERSTACK+1)*PGSIZE and p->stack_top
  uint64 stack_bottom = p->stack_top - (USERSTACK+1)*PGSIZE;
  if(va >= stack_bottom && va < p->stack_top) {
    valid = 1; 
    cause = "stack";
  } else if(va >= p->heap_start && va < stack_bottom) {
    valid = 1; 
    cause = "heap";
  } else if((va >= p->text_start && va < p->text_end) ||
            (va >= p->data_start && va < p->data_end)) {
    valid = 1; 
    cause = "exec";
  }
  
  if(!valid) {
    printf("invalid\n");
    // Don't kill process here - let caller decide
    // When called from copyin/copyout, we just want to fail the syscall
    // When called from trap handler, the trap handler will kill the process
    return -1;
  }
  
  printf("%s\n", cause);
  
  // Allocate physical page, with eviction if needed
  uint64 mem = (uint64)kalloc();
  if(mem == 0) {
    // Try to evict a page to make room
    if(lazy_evict_page(p) > 0) {
      // Retry allocation after eviction
      mem = (uint64)kalloc();
    }
    
    if(mem == 0) {
      printf("[pid %d] MEMFULL\n", p->pid);
      setkilled(p);
      return -1;
    }
  }
  
  memset((void *)mem, 0, PGSIZE);
  
  // Load from executable if this is an exec segment
  if(cause[0] == 'e' && p->exec_inode) {
    int page_idx = (va / PGSIZE) % MAX_PROC_PAGES;
    uint64 file_offset = (page_idx < MAX_PROC_PAGES) ? p->exec_off[page_idx] : 0;
    int read_len = (page_idx < MAX_PROC_PAGES) ? p->exec_len[page_idx] : 0;
    
    if(file_offset > 0 && read_len > 0) {
      ilock(p->exec_inode);
      readi(p->exec_inode, 0, mem, file_offset, read_len);
      iunlock(p->exec_inode);
      // Rest of page already zeroed by memset above
    }
  }
  
  // Set appropriate permissions by region
  int pte_flags = PTE_U | PTE_R;
  if(va >= p->text_start && va < p->text_end) {
    // Text: RX only
    pte_flags |= PTE_X;
  } else {
    // Data/heap/stack: RW
    pte_flags |= PTE_W;
  }
  
  if(mappages(p->pagetable, va, PGSIZE, mem, pte_flags) != 0) {
    kfree((void *)mem);
    setkilled(p);
    return -1;
  }
  
  // Log allocation
  if(cause[0] == 'e') {
    printf("[pid %d] LOADEXEC va=0x%lx\n", p->pid, va);
  } else {
    printf("[pid %d] ALLOC va=0x%lx\n", p->pid, va);
  }
  
  // Update page info
  struct page_info *pi = get_page_info(p, va);
  if(pi) {
    pi->va = va;
    pi->state = RESIDENT;
    pi->is_dirty = write_fault ? 1 : 0;  // Mark as dirty if write fault
    pi->seq = p->next_fifo_seq++;
    pi->swap_slot = -1;
    
    printf("[pid %d] RESIDENT va=0x%lx seq=%d\n", p->pid, va, pi->seq);
  }
  
  return 0;
}

int
alloc_swap_slot(struct proc *p)
{
  // Allocate a swap slot from the bitmap
  // Each uint64 holds 64 bits, so 1024 slots need 16 uint64s
  if(p->num_swapped_pages >= MAX_SWAP_SLOTS) {
    return -1; // No slots available
  }
  
  for(int i = 0; i < 16; i++) {
    for(int j = 0; j < 64; j++) {
      int slot_num = i * 64 + j;
      if(slot_num >= MAX_SWAP_SLOTS) break;
      
      if(!(p->swap_slot_bitmap[i] & (1UL << j))) {
        // Slot is free, allocate it
        p->swap_slot_bitmap[i] |= (1UL << j);
        return slot_num;
      }
    }
  }
  
  return -1; // No free slots
}

void
free_swap_slot(struct proc *p, int slot)
{
  if(slot < 0 || slot >= MAX_SWAP_SLOTS) return;
  
  int i = slot / 64;
  int j = slot % 64;
  
  if(i >= 0 && i < 16) {
    p->swap_slot_bitmap[i] &= ~(1UL << j);
  }
}

int
create_swap_file(struct proc *p)
{
  // If swap file already exists, don't create another one
  if(p->swapfile_inode != 0) {
    return 0;  // Already have a swap file
  }
  
  // Create per-process swap file: /pgswpPID
  // For simplicity, we'll just store the inode and use it for read/write
  char path[32];
  
  // Construct path "/pgswpXXXXX"
  int pid = p->pid;
  int len = 0;
  int pid_copy = pid;
  while(pid_copy) {
    len++;
    pid_copy /= 10;
  }
  
  path[0] = '/';
  path[1] = 'p';
  path[2] = 'g';
  path[3] = 's';
  path[4] = 'w';
  path[5] = 'p';
  
  // Write digits in reverse
  for(int i = len - 1; i >= 0; i--) {
    path[6 + i] = '0' + (pid % 10);
    pid /= 10;
  }
  path[6 + len] = '\0';
  
  begin_op();
  
  // Create the file if it doesn't exist
  struct inode *ip = create(path, T_FILE, 0, 0);
  if(ip == 0) {
    end_op();
    return -1;
  }
  
  // create() returns a locked inode, so we need to unlock it
  iunlock(ip);
  
  p->swapfile_inode = ip;
  
  end_op();
  return 0;
}

void
delete_swap_file(struct proc *p)
{
  if(p->swapfile_inode == 0) return;
  
  // Defensive: check if inode is valid BEFORE doing any filesystem operations
  // In some error cases, the inode might be partially initialized
  struct inode *ip = p->swapfile_inode;
  if(ip->ref == 0 || ip->type == 0) {
    // Inode is invalid, just clear the pointer
    p->swapfile_inode = 0;
    return;
  }
  
  // Close and delete the swap file
  begin_op();
  ilock(ip);
  ip->nlink = 0;
  iupdate(ip);  // Write changes to disk
  iunlock(ip);
  iput(ip);
  end_op();
  
  p->swapfile_inode = 0;
}

void
demand_paging_init(struct proc *p)
{
  // Alias for lazy_init
  lazy_init(p);
}

int
lazy_evict_page(struct proc *p)
{
  // FIFO page eviction: find page with minimum seq number
  int victim_idx = -1;
  uint32 min_seq = 0xffffffff;
  
  // Find resident page with minimum sequence number
  for(int i = 0; i < MAX_PROC_PAGES; i++) {
    if(p->pages[i].state == RESIDENT && p->pages[i].seq < min_seq) {
      min_seq = p->pages[i].seq;
      victim_idx = i;
    }
  }
  
  if(victim_idx == -1) {
    return -1; // No resident pages to evict
  }
  
  uint64 victim_va = p->pages[victim_idx].va;
  
  printf("[pid %d] VICTIM va=0x%lx seq=%d\n", p->pid, victim_va, p->pages[victim_idx].seq);
  
  // Check if page is dirty (tracked in page_info)
  int is_dirty = p->pages[victim_idx].is_dirty;
  
  // Check if it's an executable page (has file backing)
  int is_executable = (victim_va >= p->text_start && victim_va < p->text_end);
  
  if(is_dirty || !is_executable) {
    // Need to write to swap
    if(p->swapfile_inode == 0) {
      if(create_swap_file(p) != 0) {
        printf("[pid %d] KILL swap-exhausted\n", p->pid);
        setkilled(p);
        return -1;
      }
    }
    
    int slot = alloc_swap_slot(p);
    if(slot < 0) {
      printf("[pid %d] KILL swap-exhausted\n", p->pid);
      setkilled(p);
      return -1;
    }
    
    // Write page to swap
    uint64 pa = walkaddr(p->pagetable, victim_va);
    if(pa) {
      ilock(p->swapfile_inode);
      writei(p->swapfile_inode, 0, pa, (uint64)slot * PGSIZE, PGSIZE);
      iunlock(p->swapfile_inode);
      
      printf("[pid %d] SWAPOUT va=0x%lx slot=%d\n", p->pid, victim_va, slot);
    }
    
    p->pages[victim_idx].state = SWAPPED;
    p->pages[victim_idx].swap_slot = slot;
    p->num_swapped_pages++;
  } else {
    // Executable page with no writes - can just discard
    printf("[pid %d] DISCARD va=0x%lx\n", p->pid, victim_va);
    p->pages[victim_idx].state = UNMAPPED;
    p->pages[victim_idx].swap_slot = -1;
  }
  
  // Unmap the page using uvmunmap
  uvmunmap(p->pagetable, victim_va, PGSIZE, 1);
  
  printf("[pid %d] EVICT va=0x%lx\n", p->pid, victim_va);
  
  return 1; // Successfully evicted one page
}
