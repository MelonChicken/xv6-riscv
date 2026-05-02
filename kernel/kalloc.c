// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "stddef.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}


// (PROJECT_01 meminfo())
int
kfreemem(void)
{
  struct run *r;
  int pages = 0;
  acquire(&kmem.lock);
  r = kmem.freelist; 
  while(r){
    pages++;
    r = r -> next;
  }
  release(&kmem.lock);
  return pages * PGSIZE;
}

// PROJECT_03 kmmap()
void *
kmmap(void* addr, int length)
{
  struct run *r;
  if(((uint64)addr % PGSIZE) != 0 || (length % PGSIZE) != 0)
    return 0;
  int start = (uint64)addr / PGSIZE;
  int last = start + length / PGSIZE;
  void* startaddr = NULL; //allocated first page address
  int retptrflag = 0; //check address to return

  acquire(&kmem.lock);
  r = kmem.freelist;
  for(int i=0;i<last;i++){
    // printf("i = %d", i);
    // if r == 0, which mean the end of free list, return 0
    if(r == NULL){
      release(&kmem.lock);
      return 0;
    }
    if(i<start){
      r = r->next;
      continue;
    }
    if(r){
      kmem.freelist = r->next;
    } else {
      release(&kmem.lock);
      return 0; //page not available
    }
    if(r) {

      memset((char*)r, 5, PGSIZE); //fill with junk
    }
    if(retptrflag == 0){
      retptrflag = 1;
      startaddr = r;
    }
  }
  release(&kmem.lock);
  //if(r)
    //memset((char*)r, 5, PGSIZE); //fill with junk
  return (void*)startaddr;
}
