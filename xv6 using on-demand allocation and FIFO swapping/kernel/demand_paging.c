//
// demand_paging.c - Demand paging and swapping implementation for xv6
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "stat.h"
#include "memstat.h"

// Initialize demand paging metadata for a process
void
demand_paging_init(struct proc *p)
{
  p->text_start = 0;
  p->text_end = 0;
  p->data_start = 0;
  p->data_end = 0;
  p->heap_start = 0;
  p->stack_top = 0;
  p->next_fifo_seq = 0;
  p->swapfile = 0;
  p->num_swapped_pages = 0;
  p->num_pages = 0;
  p->exec_inode = 0;
  
  // Clear swap slot bitmap
  for(int i = 0; i < 32; i++)
    p->swap_slot_bitmap[i] = 0;
  
  // Clear page info
  for(int i = 0; i < MAX_PROC_PAGES; i++) {
    p->pages[i].va = 0;
    p->pages[i].state = UNMAPPED;
    p->pages[i].is_dirty = 0;
    p->pages[i].seq = -1;
    p->pages[i].swap_slot = -1;
    p->exec_off[i] = 0;
  }
}

// Find or create page info entry for a virtual address
struct page_info*
get_page_info(struct proc *p, uint64 va)
{
  va = PGROUNDDOWN(va);
  
  // Look for existing entry
  for(int i = 0; i < p->num_pages; i++) {
    if(p->pages[i].va == va)
      return &p->pages[i];
  }
  
  // Create new entry if space available
  if(p->num_pages < MAX_PROC_PAGES) {
    int i = p->num_pages++;
    p->pages[i].va = va;
    p->pages[i].state = UNMAPPED;
    p->pages[i].is_dirty = 0;
    p->pages[i].seq = -1;
    p->pages[i].swap_slot = -1;
    return &p->pages[i];
  }
  
  return 0; // No space
}

// Allocate a swap slot
int
alloc_swap_slot(struct proc *p)
{
  for(int i = 0; i < 1024; i++) {
    int word = i / 32;
    int bit = i % 32;
    if((p->swap_slot_bitmap[word] & (1 << bit)) == 0) {
      p->swap_slot_bitmap[word] |= (1 << bit);
      return i;
    }
  }
  return -1; // No free slots
}

// Free a swap slot
void
free_swap_slot(struct proc *p, int slot)
{
  if(slot < 0 || slot >= 1024)
    return;
  int word = slot / 32;
  int bit = slot % 32;
  p->swap_slot_bitmap[word] &= ~(1 << bit);
}

// Create swap file for process
int
create_swap_file(struct proc *p)
{
  char path[32];
  
  // Generate swap file path: /pgswpXXXXX
  path[0] = '/';
  path[1] = 'p';
  path[2] = 'g';
  path[3] = 's';
  path[4] = 'w';
  path[5] = 'p';
  
  // Convert PID to 5-digit zero-padded string
  int pid = p->pid;
  for(int i = 10; i >= 6; i--) {
    path[i] = '0' + (pid % 10);
    pid /= 10;
  }
  path[11] = '\0';
  
  begin_op();
  
  // Create the swap file
  struct inode *ip = namei(path);
  if(ip != 0) {
    // File already exists, remove it first
    iunlockput(ip);
    // TODO: implement file deletion if needed
  }
  
  // Create new file
  ip = create(path, T_FILE, 0, 0);
  if(ip == 0) {
    end_op();
    return -1;
  }
  
  // Allocate a file structure
  struct file *f = filealloc();
  if(f == 0) {
    iunlockput(ip);
    end_op();
    return -1;
  }
  
  f->type = FD_INODE;
  f->off = 0;
  f->ip = ip;
  f->readable = 1;
  f->writable = 1;
  
  iunlock(ip);
  end_op();
  
  p->swapfile = f;
  return 0;
}

// Delete swap file for process
void
delete_swap_file(struct proc *p)
{
  if(p->swapfile == 0)
    return;
  
  // Count freed slots
  int freed_slots = 0;
  for(int i = 0; i < 1024; i++) {
    int word = i / 32;
    int bit = i % 32;
    if(p->swap_slot_bitmap[word] & (1 << bit))
      freed_slots++;
  }
  
  printf("[pid %d] SWAPCLEANUP freed_slots=%d\n", p->pid, freed_slots);
  
  // Close the file
  fileclose(p->swapfile);
  p->swapfile = 0;
  
  // The file will be deleted when its link count reaches zero
  // For now, we just close it
}

// Write a page to swap
int
swap_out_page(struct proc *p, uint64 va, uint64 pa)
{
  if(p->swapfile == 0)
    return -1;
  
  struct page_info *pi = get_page_info(p, va);
  if(pi == 0)
    return -1;
  
  // Allocate a swap slot
  int slot = alloc_swap_slot(p);
  if(slot < 0) {
    printf("[pid %d] SWAPFULL\n", p->pid);
    printf("[pid %d] KILL swap-exhausted\n", p->pid);
    return -1;
  }
  
  // Write the page to swap file at the slot offset
  begin_op();
  ilock(p->swapfile->ip);
  
  uint64 offset = slot * PGSIZE;
  if(writei(p->swapfile->ip, 0, pa, offset, PGSIZE) != PGSIZE) {
    iunlock(p->swapfile->ip);
    end_op();
    free_swap_slot(p, slot);
    return -1;
  }
  
  iunlock(p->swapfile->ip);
  end_op();
  
  pi->swap_slot = slot;
  pi->state = SWAPPED;
  p->num_swapped_pages++;
  
  printf("[pid %d] SWAPOUT va=0x%lx slot=%d\n", p->pid, PGROUNDDOWN(va), slot);
  
  return slot;
}

// Read a page from swap
int
swap_in_page(struct proc *p, uint64 va, uint64 pa)
{
  if(p->swapfile == 0)
    return -1;
  
  struct page_info *pi = get_page_info(p, va);
  if(pi == 0 || pi->state != SWAPPED)
    return -1;
  
  int slot = pi->swap_slot;
  if(slot < 0)
    return -1;
  
  // Read the page from swap file
  begin_op();
  ilock(p->swapfile->ip);
  
  uint64 offset = slot * PGSIZE;
  if(readi(p->swapfile->ip, 0, pa, offset, PGSIZE) != PGSIZE) {
    iunlock(p->swapfile->ip);
    end_op();
    return -1;
  }
  
  iunlock(p->swapfile->ip);
  end_op();
  
  printf("[pid %d] SWAPIN va=0x%lx slot=%d\n", p->pid, PGROUNDDOWN(va), slot);
  
  // Free the swap slot
  free_swap_slot(p, slot);
  pi->swap_slot = -1;
  pi->state = RESIDENT;
  if(p->num_swapped_pages > 0)
    p->num_swapped_pages--;
  
  // Assign FIFO sequence number
  pi->seq = p->next_fifo_seq++;
  pi->is_dirty = 0; // Page just loaded is clean
  
  printf("[pid %d] RESIDENT va=0x%lx seq=%d\n", p->pid, PGROUNDDOWN(va), pi->seq);
  
  return 0;
}

// Find victim page using FIFO
uint64
find_fifo_victim(struct proc *p)
{
  int min_seq = -1;
  uint64 victim_va = 0;
  
  for(int i = 0; i < p->num_pages; i++) {
    if(p->pages[i].state == RESIDENT) {
      if(min_seq == -1 || p->pages[i].seq < min_seq) {
        min_seq = p->pages[i].seq;
        victim_va = p->pages[i].va;
      }
    }
  }
  
  return victim_va;
}

// Evict a page
int
evict_page(struct proc *p, uint64 victim_va)
{
  struct page_info *pi = get_page_info(p, victim_va);
  if(pi == 0 || pi->state != RESIDENT)
    return -1;
  
  // Get physical address
  pte_t *pte = walk(p->pagetable, victim_va, 0);
  if(pte == 0 || (*pte & PTE_V) == 0)
    return -1;
  
  uint64 pa = PTE2PA(*pte);
  
  // Log victim selection
  printf("[pid %d] VICTIM va=0x%lx seq=%d algo=FIFO\n", p->pid, victim_va, pi->seq);
  
  // Check if dirty
  if(pi->is_dirty) {
    printf("[pid %d] EVICT va=0x%lx state=dirty\n", p->pid, victim_va);
    
    // Swap out the page
    if(swap_out_page(p, victim_va, pa) < 0)
      return -1;
  } else {
    printf("[pid %d] EVICT va=0x%lx state=clean\n", p->pid, victim_va);
    printf("[pid %d] DISCARD va=0x%lx\n", p->pid, victim_va);
    pi->state = UNMAPPED;
  }
  
  // Unmap the page and free physical memory
  *pte = 0;
  kfree((void*)pa);
  
  return 0;
}

// Handle a page fault
// Returns 0 on success, -1 on failure (should kill process)
int
handle_page_fault(struct proc *p, uint64 va, int is_write)
{
  va = PGROUNDDOWN(va);
  
  // Determine access type for logging
  char *access_type;
  if(is_write)
    access_type = "write";
  else {
    // Check if it's an instruction fetch
    if(va >= p->text_start && va < p->text_end)
      access_type = "exec";
    else
      access_type = "read";
  }
  
  // Determine the cause/source of the fault
  char *cause = "unknown";
  int is_valid = 0;
  
  struct page_info *pi = get_page_info(p, va);
  
  // Check if page was swapped out
  if(pi && pi->state == SWAPPED) {
    cause = "swap";
    is_valid = 1;
  }
  // Check if in text/data segment
  else if(va >= p->text_start && va < p->data_end) {
    cause = "exec";
    is_valid = 1;
  }
  // Check if in heap
  else if(va >= p->heap_start && va < p->sz) {
    cause = "heap";
    is_valid = 1;
  }
  // Check if in stack (within one page below SP)
  else if(va < p->stack_top && va >= p->stack_top - 2*PGSIZE) {
    cause = "stack";
    is_valid = 1;
  }
  
  // Log the page fault
  printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=%s\n", 
         p->pid, va, access_type, cause);
  
  // If invalid access, kill the process
  if(!is_valid) {
    printf("[pid %d] KILL invalid-access va=0x%lx access=%s\n", 
           p->pid, va, access_type);
    return -1;
  }
  
  // Try to allocate physical memory
  void *mem = kalloc();
  
  // If allocation failed, we need to evict a page
  if(mem == 0) {
    printf("[pid %d] MEMFULL\n", p->pid);
    
    // Find a victim page using FIFO
    uint64 victim_va = find_fifo_victim(p);
    if(victim_va == 0) {
      // No resident pages to evict? This shouldn't happen
      return -1;
    }
    
    // Evict the victim
    if(evict_page(p, victim_va) < 0) {
      return -1;
    }
    
    // Try to allocate again
    mem = kalloc();
    if(mem == 0) {
      // Still no memory? Give up
      return -1;
    }
  }
  
  memset(mem, 0, PGSIZE);
  uint64 pa = (uint64)mem;
  
  // Handle different cases
  if(pi && pi->state == SWAPPED) {
    // Swap in the page
    if(swap_in_page(p, va, pa) < 0) {
      kfree(mem);
      return -1;
    }
  }
  else if(va >= p->text_start && va < p->data_end) {
    // Load from executable
    if(p->exec_inode == 0) {
      kfree(mem);
      return -1;
    }
    
    // Find the file offset for this page
    uint64 offset = 0;
    int found = 0;
    for(int i = 0; i < p->num_pages; i++) {
      if(p->pages[i].va == va) {
        offset = p->exec_off[i];
        found = 1;
        break;
      }
    }
    
    if(found && offset > 0) {
      // Read from executable
      begin_op();
      ilock(p->exec_inode);
      
      // Calculate how much to read (might be less than PGSIZE at end)
      uint to_read = PGSIZE;
      if(readi(p->exec_inode, 0, pa, offset, to_read) < 0) {
        iunlock(p->exec_inode);
        end_op();
        kfree(mem);
        return -1;
      }
      
      iunlock(p->exec_inode);
      end_op();
    }
    
    printf("[pid %d] LOADEXEC va=0x%lx\n", p->pid, va);
    
    // Update page info
    if(!pi)
      pi = get_page_info(p, va);
    if(pi) {
      pi->state = RESIDENT;
      pi->seq = p->next_fifo_seq++;
      pi->is_dirty = 0;
    }
    
    printf("[pid %d] RESIDENT va=0x%lx seq=%d\n", p->pid, va, pi ? pi->seq : -1);
  }
  else {
    // Heap or stack - allocate zero-filled page
    printf("[pid %d] ALLOC va=0x%lx\n", p->pid, va);
    
    // Update page info
    if(!pi)
      pi = get_page_info(p, va);
    if(pi) {
      pi->state = RESIDENT;
      pi->seq = p->next_fifo_seq++;
      pi->is_dirty = 0;
    }
    
    printf("[pid %d] RESIDENT va=0x%lx seq=%d\n", p->pid, va, pi ? pi->seq : -1);
  }
  
  // Map the page in the page table
  int perm = PTE_U | PTE_R; // Start with read-only to trap writes
  if(va >= p->text_start && va < p->text_end)
    perm |= PTE_X;
  
  // Don't set PTE_W yet - we'll catch the write fault to mark dirty
  
  if(mappages(p->pagetable, va, PGSIZE, pa, perm) < 0) {
    kfree(mem);
    return -1;
  }
  
  return 0;
}

// Handle a write to a read-only page to track dirty bit
int
handle_write_fault(struct proc *p, uint64 va)
{
  va = PGROUNDDOWN(va);
  
  struct page_info *pi = get_page_info(p, va);
  if(!pi || pi->state != RESIDENT)
    return -1;
  
  // Mark the page as dirty
  pi->is_dirty = 1;
  
  // Update page table entry to allow writes
  pte_t *pte = walk(p->pagetable, va, 0);
  if(pte == 0 || (*pte & PTE_V) == 0)
    return -1;
  
  *pte |= PTE_W;
  
  return 0;
}


