#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  if(t == SBRK_EAGER || n < 0) {
    if(growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if(addr + n < addr)
      return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

// Get memory statistics for the current process
uint64
sys_memstat(void)
{
  uint64 addr;
  struct proc *p = myproc();
  struct proc_mem_stat info;
  
  argaddr(0, &addr);
  
  // Fill in the memory statistics
  info.pid = p->pid;
  info.next_fifo_seq = p->next_fifo_seq;
  
  // Count pages by state
  int num_resident = 0;
  int num_swapped = 0;
  int num_total = 0;
  
  // Calculate total number of virtual pages
  uint64 total_va_space = p->sz;
  num_total = total_va_space / PGSIZE;
  if(total_va_space % PGSIZE != 0)
    num_total++;
  
  // Count by examining page info
  for(int i = 0; i < p->num_pages && i < MAX_PAGES_INFO; i++) {
    if(p->pages[i].state == RESIDENT)
      num_resident++;
    else if(p->pages[i].state == SWAPPED)
      num_swapped++;
  }
  
  info.num_pages_total = num_total;
  info.num_resident_pages = num_resident;
  info.num_swapped_pages = num_swapped;
  
  // Copy page information (up to MAX_PAGES_INFO)
  int pages_to_copy = p->num_pages < MAX_PAGES_INFO ? p->num_pages : MAX_PAGES_INFO;
  for(int i = 0; i < pages_to_copy; i++) {
    info.pages[i].va = (uint)p->pages[i].va;
    info.pages[i].state = p->pages[i].state;
    info.pages[i].is_dirty = p->pages[i].is_dirty;
    info.pages[i].seq = p->pages[i].seq;
    info.pages[i].swap_slot = p->pages[i].swap_slot;
  }
  
  // Copy the structure to user space
  if(copyout(p->pagetable, addr, (char*)&info, sizeof(info)) < 0)
    return -1;
  
  return 0;
}
