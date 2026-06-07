#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "vm.h"


// PROJECT 04
#include "page.h"

#define NPHYSPAGES ((PHYSTOP - KERNBASE) / PGSIZE)
//char debug_is_pagetable_page[NPHYSPAGES];


/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S
//Project 03
extern struct mmap_area* is_in_mmap_area(struct proc *p, uint64 va);

extern int setoff(struct file *f, int off); // from file.c



// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  //Check va validity
  if(va >= MAXVA)
    panic("walk");
  // PROJECT 04 DEBUG: Check pagetable validity
  /*
  if(pagetable == 0 || (uint64)pagetable < KERNBASE || (uint64)pagetable >= PHYSTOP){
    printf("[walk] bad root pagetable=%p va=%p alloc=%d\n", (void *)pagetable, (void *)va, alloc);
    panic("walk bad root pagetable");
  }
  */


  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    // PROJECT 04 DEBUG Check pte validity
    /*
    if((uint64)pte < KERNBASE || (uint64)pte >= PHYSTOP){
      printf("[walk] bad pte pointer pte=%p pagetable=%p va=%p level=%d\n", (void *)pte, (void *)pagetable, (void *)va, level);
      panic("walk bad pte pointer");
    }
    */

    if(*pte & PTE_V) {
      pagetable_t next = (pagetable_t)PTE2PA(*pte);

      // PROJECT 04 DEBUG Check the next pagetable to be accessed.
      /*
      if((uint64)next < KERNBASE || (uint64)next >= PHYSTOP){
        printf("[walk] bad next pagetable current_pt=%p pte=%p next=%p va=%p level=%d\n", (void *)pagetable, (void *)*pte, (void *)next, (void *)va, level);
        panic("walk bad next pagetable");
      }
      */

      pagetable = next;
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;

      memset(pagetable, 0, PGSIZE);

      //PROJECT 04 DEBUG Mark the new page-table page
      /*
      int idx = ((uint64)pagetable - KERNBASE) / PGSIZE;
      if(idx >= 0 && idx < NPHYSPAGES){
        debug_is_pagetable_page[idx] = 1;
        printf("[debug] new page-table page pa=%p idx=%d\n", (void *)pagetable, idx);
      }
      */

      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  //PROJECT 04 DEBUG Finalized page table check
  /*
  if((uint64)pagetable < KERNBASE || (uint64)pagetable >= PHYSTOP){
    printf("[walk] bad final pagetable=%p va=%p\n", (void *)pagetable, (void *)va);
    panic("walk bad final pagetable");
  }
  */
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

  // Create PTEs for virtual addresses starting at va that refer to
  // physical addresses starting at pa.
  // va and size MUST be page-aligned.
  // Returns 0 on success, -1 if walk() couldn't
  // allocate a needed page-table page.
  int
  mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
  {
    uint64 a, last;
    pte_t *pte;
    if((va % PGSIZE) != 0)
      panic("mappages: va not aligned");

    if((size % PGSIZE) != 0)
      panic("mappages: size not aligned");

    if(size == 0)
      panic("mappages: size");
    
    a = va;
    last = va + size - PGSIZE;
    for(;;){
      if((pte = walk(pagetable, a, 1)) == 0)
        return -1;
      if(*pte & PTE_V)
        panic("mappages: remap");
      if(*pte & PTE_S){
        return -1;
      }
      *pte = PA2PTE(pa) | perm | PTE_V;
      //PROJECT 04: Addition of pages to the LRU list
      if((*pte & PTE_U) && (*pte & (PTE_R|PTE_W|PTE_X)) && a != TRAMPOLINE && a != TRAPFRAME){
        lru_add(pagetable, a, pa);
      }
      if(a == last)
        break;

      a += PGSIZE;
      pa += PGSIZE;
    }
    return 0;
  }

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  /* PROJECT 04 DEBUG
  int idx = ((uint64)pagetable - KERNBASE) / PGSIZE;

  if(idx >= 0 && idx < (PHYSTOP - KERNBASE) / PGSIZE){
    //debug_is_pagetable_page[idx] = 1;
    //printf("[debug] new ROOT page-table page pa=%p idx=%d\n", (void *)pagetable, idx);
  }
  */

  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0) // leaf page table entry allocated?
      continue;
    if((*pte & PTE_S) && ((*pte & PTE_V) == 0)){ // is the page swapped? -> needs to be cleared from swap space
      int blkno = PTE2BLKNO(*pte);
      swapslot_free(blkno);
      *pte = 0;
      continue;
    }
    if((*pte & PTE_V) == 0)  // has physical page been allocated?
      continue;
    uint64 pa = PTE2PA(*pte);
    // PROJECT 04: Removal of pages from the LRU list
    if((*pte & PTE_U) && (*pte & (PTE_R|PTE_W|PTE_X)) && a != TRAPFRAME && a != TRAMPOLINE)
      lru_remove(pa);
    if(do_free){
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

// Allocate PTEs and physical memory to grow a process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }

  // PROJECT 04 DEBUG
  /*
  int idx = ((uint64)pagetable - KERNBASE) / PGSIZE;
  if(idx >= 0 && idx < NPHYSPAGES){
    debug_is_pagetable_page[idx] = 0;
    printf("[debug] free page-table page pa=%p idx=%d\n", (void *)pagetable, idx);
  }
  */


  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  // PROJECT 04 Remove all the pages from the lru list belonging to the process.
  lru_remove_pagetable(pagetable);
  //Checks if LRU list has pages that contain already-freed pagetable address
  lru_assert_no_pagetable_refs(pagetable);

  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}
// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      continue;   // page table entry hasn't been allocated

    // PROJECT 04:
    // If the parent's page has been swapped out,
    // PTE_V == 0 and PTE_S == 1.
    // In that case, we must swap it back in first,
    // then copy the restored physical page into the child.
    if((*pte & PTE_S) && ((*pte & PTE_V) == 0)){
      if(swap_in(old, i) < 0)
        goto err;
    }

    // If this page is neither valid nor swapped, there is no physical page to copy.
    if((*pte & PTE_V) == 0)
      continue;

    // Now the parent page is guaranteed to have a physical frame.
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);

    // Allocate a separate physical page for the child.
    // Keep the source page resident while kalloc() may trigger eviction.
    lru_remove(pa);
    if((mem = kalloc()) == 0){
      lru_add(old, i, pa);
      goto err;
    }

    // Copy parent's physical page contents into child's new physical page.
    memmove(mem, (char*)pa, PGSIZE);
    lru_add(old, i, pa);

    // Map the copied page into the child's page table.
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }

  return 0;

err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if(va0 >= MAXVA)
      return -1;
  
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }

    pte = walk(pagetable, va0, 0);
    // forbid copyout over read-only user text pages.
    if((*pte & PTE_W) == 0)
      return -1;
      
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}

// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// out of physical memory, and physical address if successful.
uint64
vmfault(pagetable_t pagetable, uint64 va, int read)
{
  
  uint64 mem;
  struct proc *p = myproc();
  struct mmap_area *a = 0;

  // move va where page fault happened to page boundary
  va = PGROUNDDOWN(va);
  
  // if already mapped, just pass
  if(ismapped(pagetable, va)) return 0;

  pte_t *pte = walk(pagetable, va, 0);

  // CASE 01: Is the page swapped out?
  if(pte != 0 && (*pte & PTE_S)){
    if(swap_in(pagetable, va) < 0)
      return 0;
    return walkaddr(pagetable, va);
  }

  // CASE 02: Normal page-fault handling
  if(va < p->sz){
    mem = (uint64) kalloc();
    if(mem == 0) return 0;

    memset((void *) mem, 0, PGSIZE);

    if(mappages(p->pagetable, va, PGSIZE, mem, PTE_W|PTE_U|PTE_R) != 0) {
      kfree((void *)mem);
      return 0;
    }
    return mem;
  }


  // CASE 03: Perhaps page fault occurred in mmap region? -> Find process's mmap_area that has va within it.
  
  a = is_in_mmap_area(p, va);
  if(a == 0) return 0;
  int perm = PTE_U;
  if(a->prot & PROT_READ) perm |= PTE_R;
  if(a->prot & PROT_WRITE) perm |= PTE_W;

  mem = (uint64) kalloc();
  if(mem==0) goto clear;
  if(mappages(p->pagetable, va, PGSIZE, mem, perm) != 0) {
    kfree((void *) mem ); // free memory
    goto clear;
  }

  //fileBacked
  if(!(a->flags&MAP_ANONYMOUS)){
    if(setoff(a->f, a->offset + va - a->addr) == -1) goto clear;
    
    int cond = fileread(a->f, va, PGSIZE);
    if(cond<0) goto clear;
  }
  return mem;

  clear:
    munmap(a->addr);
    return 0;

}

int
ismapped(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_V){
    return 1;
  }
  return 0;
}
