#include "types.h"
#include "param.h"
#include "riscv.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "buf.h"
#include "defs.h"
#include "swap.h"

int swap_used[NSWAPSLOT];
struct spinlock slock; // Protects swap_used[] from race conditions

// I/O stats (diskblocks) for PROJECT 04 TEST INIT
struct {
  struct spinlock lock;
  int nr_sectors_read;
  int nr_sectors_write;
} swapstats;

void
swapinit(void)
{
  initlock(&swapstats.lock, "swapstats"); //PROJECT 04 TEST INIT
  swapstats.nr_sectors_read = 0;
  swapstats.nr_sectors_write = 0;
  initlock(&slock, "swap");
  for(int i=0;i<NSWAPSLOT;i++){
    swap_used[i]=0;
  }
}

// Use swap_used[NSWAPSLOT] to track down which slot is available and ready to be allocated.

int
swapslot_alloc(void)
{
  // get lock for swap_used[]
  acquire(&slock);
  
  // looping swap slots
  for(int i=0;i<NSWAPSLOT;i++){
    // if it is not used
    if(swap_used[i]==0){
      // reserve this slot
      swap_used[i] = 1;
      release(&slock);
      // convert slot number into the actual block number
      int startblock = SWAPBASE + i*BLOCKPERPAGE;

      return startblock;
    }
  }
  // there is no unused slot (full!)
  release(&slock);
  return 0;
}

void
swapslot_free(int blkno)
{
  int slot = (blkno  - SWAPBASE)/BLOCKPERPAGE;
  if(slot < 0 || slot >= NSWAPSLOT)
    panic("swapslot_free: bad blkno");

  acquire(&slock);
  if(swap_used[slot] == 0) //double free exception
    panic("double free swapslot");
  swap_used[slot] = 0;
  release(&slock);
}

int
swapout(uint64 pa, int blkno)
{
  for(int i=0;i<BLOCKPERPAGE;i++){
    struct buf *b = bread(SWAPDEV, blkno + i);
    memmove(b->data,(char*)pa+i*BSIZE,BSIZE);
    bwrite(b);
    brelse(b);
  }
  acquire(&swapstats.lock);
  swapstats.nr_sectors_write += BLOCKPERPAGE;
  release(&swapstats.lock);

  return blkno;
}

void
swapin(uint64 pa, int blkno)
{
  for(int i=0;i<BLOCKPERPAGE;i++){
    struct buf *b = bread(SWAPDEV, blkno + i);
    memmove((char*)pa+i*BSIZE,b->data,BSIZE);
    brelse(b);
  }
  acquire(&swapstats.lock);
  swapstats.nr_sectors_read += BLOCKPERPAGE;
  release(&swapstats.lock);
  swapslot_free(blkno);
}

void
swapstat(int *nr_sectors_read, int *nr_sectors_write)
{
  acquire(&swapstats.lock);
  if(nr_sectors_read) *nr_sectors_read = swapstats.nr_sectors_read/4;
  if(nr_sectors_write) *nr_sectors_write = swapstats.nr_sectors_write/4;
  release(&swapstats.lock);
}

void *
swap_out(void)
{
  pagetable_t pt = 0;
  uint64 va = 0;

  uint64 pa = lru_select_victim(&pt, &va);
  if(pa == 0)
    return 0;

  int blkno = swapslot_alloc();
  if(blkno == 0){
    lru_add(pt, va, pa);
    return 0;
  }

  pte_t *pte = walk(pt, va, 0);
  if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0 || PTE2PA(*pte) != pa){
    swapslot_free(blkno);
    lru_add(pt, va, pa);
    return 0;
  }

  uint flags = PTE_FLAGS(*pte);

  swapout(pa, blkno);

  flags &= ~PTE_V;
  flags &= ~PTE_A;
  flags &= ~PTE_D;

  *pte = BLKNO2PTE(blkno) | flags | PTE_S;

  sfence_vma();

  return (void *)pa;
}

int
swap_in(pagetable_t pt, uint64 va)
{
  va = PGROUNDDOWN(va);

  pte_t *pte = walk(pt, va, 0);
  if(pte == 0)
    return -1;

  if((*pte & PTE_V) || ((*pte & PTE_S) == 0))
    return -1;

  int blkno = PTE2BLKNO(*pte);
  uint flags = PTE_FLAGS(*pte);

  char *mem = kalloc();
  if(mem == 0)
    return -1;

  swapin((uint64)mem, blkno);

  flags &= ~PTE_S;
  flags |= PTE_V;
  flags |= PTE_A;

  *pte = PA2PTE((uint64)mem) | flags;

  lru_add(pt, va, (uint64)mem);

  sfence_vma();

  return 0;
}
