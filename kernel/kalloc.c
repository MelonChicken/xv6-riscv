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

struct {
  struct spinlock lock;
  struct run *mfreelist;
} kmemmmap;

// MODIFIED FOR PROJECT 03: split pages
void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&kmemmmap.lock, "kmemmmap");
  //freerange(end, (void*)MMAPBASE);
  //freerangemmap((void*)MMAPBASE, (void*)PHYSTOP);
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  char *pformmap;
  p = (char*)PGROUNDUP((uint64)pa_start);
  pformmap = (char*)PGROUNDUP((uint64)MMAPBASE);
  for(; p + PGSIZE <= (char*)pa_end; p+= PGSIZE){
    if(p >= pformmap){
      kfree(p,1);
      continue;
    }
    kfree(p,0);
  }
}

//void
//freerangemmap(void *pa_start, void *pa_end)
//{
//  char *p;
//  p = (char*)PGROUNDUP((uint64)pa_start);
//  for(; p + PGSIZE <= (char*)pa_end; p+= PGSIZE)
//    kfree(p);
//}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa, int mmapareaflag)
{
  struct run *r;
  struct run *mr;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  //r = (struct run*)pa;

  if(mmapareaflag == 1){ //Page for mmap freelist
    mr = (struct run*)pa;
    acquire(&kmemmmap.lock);
    mr->next = kmemmmap.mfreelist;
    kmemmmap.mfreelist = mr;
    release(&kmemmmap.lock);
  }
  else if(mmapareaflag == 0)  //Page for kalloc freelist
  {
    r = (struct run*)pa;
    acquire(&kmem.lock);
    r->next = kmem.freelist;
    kmem.freelist = r;
    release(&kmem.lock);
  } else { // Code should not reach here.
    panic("mmapareaflag");
  }
  //acquire(&kmem.lock);
  //r->next = kmem.freelist;
  //kmem.freelist = r;
  //release(&kmem.lock);
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

// TODO: Project 03 mmap 
void *
kmmap(int length)
{
  //length has to be multiple of PGSIZE
  if(length % PGSIZE > 0) {
    panic("kmmap length");
  }

  void *start = NULL; //The first page addr
  int startflag = 0; //When to fetch first page addr
  
  
  struct run *mr;
  //int mmapbase = MMAPBASE / PGSIZE;
  int pages = length / PGSIZE;
  
  acquire(&kmemmmap.lock);
  mr = kmemmmap.mfreelist;
  for(int i=0;i<pages;i++){
    if(mr){
      kmemmmap.mfreelist = mr->next;
    }
    else{
      release(&kmemmmap.lock);
      return 0; //not enough free pages in kmemmmap.freelist.
    }
    if(mr)
      memset((char*)mr, 5, PGSIZE); //fill with junk
    if(startflag==0){
      startflag = 1;
      start = mr;
    }
  }
  release(&kmemmmap.lock);
  if((void*)start==NULL)
    panic("mmap:Bro start addr missing");
  
  return (void*)start;
  //if(mr)
    //memset((char*)mr, 5, PGSIZE);
  
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
