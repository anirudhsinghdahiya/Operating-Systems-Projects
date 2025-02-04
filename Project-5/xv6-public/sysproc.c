#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"    
#include "sleeplock.h"   
#include "wmap.h"
#include "fs.h"
#include "file.h"

int
sys_fork(void)
{
  return fork();
}

int
sys_exit(void)
{
  exit();
  return 0;  // not reached
}

int
sys_wait(void)
{
  return wait();
}

int
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

int
sys_getpid(void)
{
  return myproc()->pid;
}

int
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

int
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

int
sys_wmap(void)
{
  uint addr;
  int length, flags, fd;
  
  if(argint(1, &length) < 0 || argint(2, &flags) < 0 || argint(3, &fd) < 0 ||
     argint(0, (int*)&addr) < 0)
    return FAILED;

  // Basic validation
  if(length <= 0)
    return FAILED;
  
  if((flags & MAP_FIXED) == 0)
    return FAILED;

  if((flags & MAP_SHARED) == 0)
    return FAILED;

  // Address range check
  if(addr < 0x60000000 || addr >= 0x80000000 || addr % PGSIZE != 0)
    return FAILED;

  struct proc *p = myproc();
  int i;

  // Find free mapping slot
  for(i = 0; i < NMMAPS; i++) {
    if(!p->mmaps[i].used)
      break;
  }
  if(i == NMMAPS)
    return FAILED;

  // Check if address range is available
  for(int j = 0; j < NMMAPS; j++) {
    if(p->mmaps[j].used) {
      if((addr >= p->mmaps[j].addr && 
          addr < p->mmaps[j].addr + p->mmaps[j].length) ||
         (addr + length > p->mmaps[j].addr && 
          addr + length <= p->mmaps[j].addr + p->mmaps[j].length))
        return FAILED;
    }
  }

  // Set up mapping
  p->mmaps[i].addr = addr;
  p->mmaps[i].length = length;
  p->mmaps[i].flags = flags;
  p->mmaps[i].used = 1;
  p->mmaps[i].offset = 0;
  p->mmaps[i].ip = 0;

  // Handle file-backed mapping
  if(!(flags & MAP_ANONYMOUS)) {
    if(fd < 0 || fd >= NOFILE || (p->ofile[fd] == 0))
      return FAILED;
    p->mmaps[i].ip = p->ofile[fd]->ip;
    // Increment ref count on inode
    idup(p->mmaps[i].ip);
  }

  p->total_mmaps++;
  return addr;
}

int
sys_wunmap(void)
{
  uint addr;
  if(argint(0, (int*)&addr) < 0)
    return FAILED;

  struct proc *p = myproc();
  int i;

  // Find the mapping
  for(i = 0; i < NMMAPS; i++) {
    if(p->mmaps[i].used && p->mmaps[i].addr == addr)
      break;
  }
  if(i == NMMAPS)
    return FAILED;

  // File-backed mapping: write back first if needed
  if(!(p->mmaps[i].flags & MAP_ANONYMOUS) && (p->mmaps[i].flags & MAP_SHARED)) {   
   uint chunk_size = 512;  // Standard block size
   uint remaining = p->mmaps[i].length;
   uint current_addr = addr;
   
   while(remaining > 0) {
       begin_op();
       ilock(p->mmaps[i].ip);
       
       uint write_size = (remaining < chunk_size) ? remaining : chunk_size;
       pte_t *pte = walkpgdir(p->pgdir, (void*)current_addr, 0);
       
       if(pte && (*pte & PTE_P)) {
           char *mem = P2V(PTE_ADDR(*pte));
           uint offset = current_addr - p->mmaps[i].addr;           
           
           if(writei(p->mmaps[i].ip, mem + (offset % PGSIZE), offset, write_size) != write_size) {
               iunlock(p->mmaps[i].ip);
               end_op();
               return FAILED;
           }
       }
       
       iunlock(p->mmaps[i].ip);
       end_op();
       
       remaining -= write_size;
       current_addr += write_size;
   }
   
}

  // Clean up memory and page table entries
  pte_t *pte;
  uint va;
  for(va = addr; va < addr + p->mmaps[i].length; va += PGSIZE) {
    if((pte = walkpgdir(p->pgdir, (void*)va, 0)) != 0 && (*pte & PTE_P)) {
      char *v = P2V(PTE_ADDR(*pte));
      kfree(v);
      *pte = 0;
    }
  }

  // Release file if needed
  if(!(p->mmaps[i].flags & MAP_ANONYMOUS)) {
    begin_op();
    iput(p->mmaps[i].ip);
    end_op();
  }

  memset(&p->mmaps[i], 0, sizeof(p->mmaps[i]));
  p->total_mmaps--;
  return SUCCESS;
}

uint
sys_va2pa(void)
{
  uint va;
  if(argint(0, (int*)&va) < 0)
    return -1;

  pte_t *pte;
  struct proc *p = myproc();

  if((pte = walkpgdir(p->pgdir, (void*)va, 0)) == 0)
    return -1;

  if(!(*pte & PTE_P))
    return -1;

  return PTE_ADDR(*pte) | (va & (PGSIZE-1));
}

int
sys_getwmapinfo(void)
{
  struct wmapinfo *wminfo;
  if(argptr(0, (char**)&wminfo, sizeof(*wminfo)) < 0)
    return FAILED;

  struct proc *p = myproc();
  wminfo->total_mmaps = p->total_mmaps;
  
  int idx = 0;
  for(int i = 0; i < NMMAPS && idx < MAX_WMMAP_INFO; i++) {
    if(p->mmaps[i].used) {
      wminfo->addr[idx] = p->mmaps[i].addr;
      wminfo->length[idx] = p->mmaps[i].length;
      
      // Count loaded pages
      int count = 0;
      uint va;
      for(va = p->mmaps[i].addr; 
          va < p->mmaps[i].addr + p->mmaps[i].length; 
          va += PGSIZE) {
        pte_t *pte;
        if((pte = walkpgdir(p->pgdir, (void*)va, 0)) != 0 && (*pte & PTE_P))
          count++;
      }
      wminfo->n_loaded_pages[idx] = count;
      idx++;
    }
  }
  
  return SUCCESS;
}