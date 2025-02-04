#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"
#include "wmap.h"
#include "mmu.h"

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];  // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

void
tvinit(void)
{
  int i;

  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);

  initlock(&tickslock, "time");
}

void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

//PAGEBREAK: 41
void
trap(struct trapframe *tf)
{
  if(tf->trapno == T_SYSCALL){
    if(myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if(myproc()->killed)
      exit();
    return;
  }

  switch(tf->trapno){
  case T_PGFLT: {
    uint va = rcr2();  // Get the faulting virtual address
    struct proc *p = myproc();
    int i;

    // Check if it's a page fault in mapped region
    for(i = 0; i < NMMAPS; i++) {
      if(p->mmaps[i].used && 
         va >= p->mmaps[i].addr && 
         va < p->mmaps[i].addr + p->mmaps[i].length) {
        
        char *mem;
        uint aligned_addr = PGROUNDDOWN(va);
        pte_t *pte = walkpgdir(p->pgdir, (void*)aligned_addr, 0);

        // Handle copy-on-write if needed
        if(pte && (*pte & PTE_P) && !(*pte & PTE_W)) {
          uint pa = PTE_ADDR(*pte);
          if(ref_counts[pa/PGSIZE] > 1) {
            if((mem = kalloc()) == 0) {
              p->killed = 1;
              break;
            }
            memmove(mem, (char*)P2V(pa), PGSIZE);
            *pte = V2P(mem) | PTE_W | PTE_U | PTE_P;
            ref_counts[pa/PGSIZE]--;
            ref_counts[V2P(mem)/PGSIZE] = 1;
            lcr3(V2P(p->pgdir));
            break;
          }
        }

        // Lazy allocation
        if((mem = kalloc()) == 0) {
          p->killed = 1;
          break;
        }
        memset(mem, 0, PGSIZE);

        // For file-backed mapping, read from file
        if(!(p->mmaps[i].flags & MAP_ANONYMOUS) && p->mmaps[i].ip) {
            begin_op();
            ilock(p->mmaps[i].ip);
            
            uint offset = aligned_addr - p->mmaps[i].addr;
            uint size = (offset + PGSIZE > p->mmaps[i].length) ? 
                        p->mmaps[i].length - offset : PGSIZE;
            
            if(readi(p->mmaps[i].ip, mem, offset, size) != size) {
                iunlock(p->mmaps[i].ip);
                end_op();
                kfree(mem);
                p->killed = 1;
                break;
            }
            
            if(size < PGSIZE)
                memset(mem + size, 0, PGSIZE - size);
            
            iunlock(p->mmaps[i].ip);
            end_op();
        } else {
            // Handle anonymous mapping
            // Get content from parent if it exists
            if(p->parent && p->parent->pgdir) {
                pte_t *parent_pte = walkpgdir(p->parent->pgdir, (void*)va, 0);
                if(parent_pte && (*parent_pte & PTE_P)) {
                    memmove(mem, (char*)P2V(PTE_ADDR(*parent_pte)), PGSIZE);
                }
            }
        }

        if(mappages(p->pgdir, (void*)aligned_addr, PGSIZE, V2P(mem), PTE_W|PTE_U|PTE_P) < 0) {
            kfree(mem);
            p->killed = 1;
        }
        break;
      }
    }

    // If address not in any mapping, kill process
    if(i == NMMAPS) {
      cprintf("Segmentation Fault\n");
      p->killed = 1;
    }
    break;
  }

  case T_IRQ0 + IRQ_TIMER:
    if(cpuid() == 0){
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpuid(), tf->cs, tf->eip);
    lapiceoi();
    break;

  //PAGEBREAK: 13
  default:
    if(myproc() == 0 || (tf->cs&3) == 0){
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpuid(), tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno,
            tf->err, cpuid(), tf->eip, rcr2());
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if(myproc() && myproc()->state == RUNNING &&
     tf->trapno == T_IRQ0+IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();
}