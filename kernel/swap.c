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

void
swapinit(void)
{
  initlock(&slock, "swap");
  for(int i=0;i<NSWAPSLOT;i++){
    swap_used[i]=0;
  }
}

// Use swap_used[NSWAPSLOT] to track down which slot is available and ready to be allocated.

int
swapslot_alloc(void)
{
  acquire(&slock);
  for(int i=0;i<NSWAPSLOT;i++){
    if(swap_used[i]==0){
      swap_used[i] = 1;
      release(&slock);
      int startblock = SWAPBASE + i*BLOCKPERPAGE;
      return startblock;
    }
  }
  release(&slock);
  return 0;
}

void
swapslot_free(int blkno)
{
  acquire(&slock);
  int slot = (blkno  - SWAPBASE)/BLOCKPERPAGE;
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
  swapslot_free(blkno);
}
