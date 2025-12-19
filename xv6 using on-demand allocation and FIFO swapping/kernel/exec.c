#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

// Removed loadseg() - no longer needed for demand paging

// map ELF permissions to PTE permission bits.
int flags2perm(int flags)
{
    int perm = 0;
    if(flags & 0x1)
      perm = PTE_X;
    if(flags & 0x2)
      perm |= PTE_W;
    return perm;
}

//
// the implementation of the exec() system call
// Modified for demand paging - does NOT pre-allocate or load program pages
//
int
kexec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();
  uint64 text_start = 0xFFFFFFFFFFFFFFFF, text_end = 0;
  uint64 data_start = 0xFFFFFFFFFFFFFFFF, data_end = 0;
  // Disabled: struct inode *old_exec_inode;
  int ip_locked = 0;  // Track if ip is locked
  
  // Disabled: Save the old exec_inode before we potentially overwrite it
  // old_exec_inode = p->exec_inode;
  
  // Clear old exec information
  for(i = 0; i < MAX_PROC_PAGES; i++) {
    p->exec_off[i] = 0;
    p->exec_len[i] = 0;
  }
  p->num_pages = 0;

  begin_op();

  // Open the executable file.
  if((ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  ip_locked = 1;

  // Read the ELF header.
  if(readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;

  // Is this really an ELF file?
  if(elf.magic != ELF_MAGIC)
    goto bad;

  if((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  // Scan program headers to determine memory layout
  // Do NOT allocate or load pages - just record the layout
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if(ph.vaddr % PGSIZE != 0)
      goto bad;
    
    // Determine if this is text or data
    int is_exec = (ph.flags & 0x1) != 0; // Executable flag
    
    if(is_exec) {
      // Text segment
      if(ph.vaddr < text_start)
        text_start = ph.vaddr;
      if(ph.vaddr + ph.memsz > text_end)
        text_end = ph.vaddr + ph.memsz;
    } else {
      // Data segment
      if(ph.vaddr < data_start)
        data_start = ph.vaddr;
      if(ph.vaddr + ph.memsz > data_end)
        data_end = ph.vaddr + ph.memsz;
    }
    
    // Record page information for demand loading
    for(uint64 va = ph.vaddr; va < ph.vaddr + ph.memsz; va += PGSIZE) {
      struct page_info *pi = get_page_info(p, va);
      if(pi) {
        pi->va = va;
        pi->state = UNMAPPED;
        // Store file offset for this page
        uint64 page_off = va - ph.vaddr;
        int page_idx = (pi - p->pages);
        
        if(page_off < ph.filesz) {
          p->exec_off[page_idx] = ph.off + page_off;
          // Calculate how many bytes to read from this page
          uint64 bytes_left = ph.filesz - page_off;
          p->exec_len[page_idx] = (bytes_left > PGSIZE) ? PGSIZE : bytes_left;
        } else {
          p->exec_off[page_idx] = 0; // BSS section
          p->exec_len[page_idx] = 0;
        }
      }
    }
    
    // Update sz to track end of program
    if(ph.vaddr + ph.memsz > sz)
      sz = ph.vaddr + ph.memsz;
  }
  
  // Keep ip locked and referenced for now
  // We'll set p->exec_inode only after we're sure exec will succeed
  
  iunlock(ip);
  ip_locked = 0;
  // Don't end_op yet - we still need the inode reference
  
  p = myproc();
  uint64 oldsz = p->sz;

  // Set up stack WITHOUT allocating physical pages
  sz = PGROUNDUP(sz);
  sp = sz + (USERSTACK+1)*PGSIZE;
  stackbase = sp - USERSTACK*PGSIZE;
  
  // Store memory layout in proc structure BEFORE any copyout operations
  // This is critical because copyout can trigger page faults during exec
  p->text_start = text_start;
  p->text_end = text_end;
  p->data_start = data_start;
  p->data_end = data_end;
  p->heap_start = PGROUNDUP(data_end);
  p->stack_top = sz + (USERSTACK+1)*PGSIZE; // Top of stack region (absolute address)
  p->sz = sz + (USERSTACK+1)*PGSIZE;
  
  // Allocate physical pages for stack temporarily to write arguments
  // These will be the only pages allocated eagerly
  char *stack_mem = kalloc();
  if(stack_mem == 0)
    goto bad;
  memset(stack_mem, 0, PGSIZE);
  
  // Map one stack page temporarily for copyout
  if(mappages(pagetable, stackbase, PGSIZE, (uint64)stack_mem, PTE_W | PTE_R | PTE_U) < 0) {
    kfree(stack_mem);
    goto bad;
  }
  
  // Track this stack page in page info
  struct page_info *stack_pi = get_page_info(p, stackbase);
  if(stack_pi) {
    stack_pi->state = RESIDENT;
    stack_pi->seq = p->next_fifo_seq++;
    stack_pi->is_dirty = 0;
  }
  
  sp = stackbase + PGSIZE;

  // Copy argument strings into stack
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // riscv sp must be 16-byte aligned
    if(sp < stackbase)
      goto bad;
    if(copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // push a copy of ustack[], the array of argv[] pointers.
  sp -= (argc+1) * sizeof(uint64);
  sp -= sp % 16;
  if(sp < stackbase)
    goto bad;
  if(copyout(pagetable, sp, (char *)ustack, (argc+1)*sizeof(uint64)) < 0)
    goto bad;

  // a0 and a1 contain arguments to user main(argc, argv)
  p->trapframe->a1 = sp;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(p->name, last, sizeof(p->name));
    
  // Commit to the user image.
  oldpagetable = p->pagetable;
  p->pagetable = pagetable;
  // NOTE: p->sz and memory layout fields were already set earlier, before copyout
  p->trapframe->epc = elf.entry;  // initial program counter = main
  p->trapframe->sp = sp; // initial stack pointer
  proc_freepagetable(oldpagetable, oldsz);
  
  // Now that we've committed, set the new exec_inode
  // We need to keep ip referenced, so call idup() to increment ref count
  // This ref will be released when the process exits
  p->exec_inode = idup(ip);
  
  // Create swap file for this process
  // DISABLED: Swapping not fully implemented, and causes issues
  // if(create_swap_file(p) < 0) {
  //   // Non-fatal - swapping just won't work
  // }
  
  // Log the lazy map initialization
  printf("[pid %d] INIT-LAZYMAP text=[0x%lx,0x%lx) data=[0x%lx,0x%lx) heap_start=0x%lx stack_top=0x%lx\n",
         p->pid, text_start, text_end, data_start, data_end, p->heap_start, p->stack_top);
  
  // Don't release old_exec_inode - just let it leak for now
  // This avoids potential ilock panics
  // if(old_exec_inode) {
  //   iput(old_exec_inode);
  // }
  
  end_op();

  return argc; // this ends up in a0, the first argument to main(argc, argv)

 bad:
  if(pagetable) {
    // Explicitly unmap stack page if it was allocated
    // This must be done before proc_freepagetable
    uint64 stackbase_calc = PGROUNDUP(sz) + PGSIZE;
    uvmunmap(pagetable, stackbase_calc, 1, 1);  // Unmap 1 page, free physical memory
    
    // Now free the page table normally
    proc_freepagetable(pagetable, 0);  // Pass 0 since we manually unmapped everything
  }
  // Release the inode - unlock if needed, then put
  if(ip){
    if(ip_locked)
      iunlock(ip);
    iput(ip);
    end_op();
  }
  return -1;
}

// Removed loadseg() function - no longer needed with demand paging
// Pages are loaded on-demand via page fault handler
