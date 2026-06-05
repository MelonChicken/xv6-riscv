#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "page.h"

struct page pages[(PHYSTOP-KERNBASE)/PGSIZE];

struct {
  struct spinlock lock;
  struct page *head;
  int count;
} lru;

static void
clear_page_meta(struct page *pg)
{
  pg->next = 0;
  pg->prev = 0;
  pg->pagetable = 0;
  pg->vaddr = 0;
  pg->age = 0;
  pg->used = 0;
}

static uint64
page_to_pa(struct page *pg)
{
  return KERNBASE + ((pg - pages) * PGSIZE);
}

void
lruinit(void)
{
  initlock(&lru.lock, "lru");
  lru.head = 0;
  lru.count = 0;

  for(int i = 0; i < (PHYSTOP-KERNBASE)/PGSIZE; i++)
    clear_page_meta(&pages[i]);
}

int
lru_size(void)
{
  int n;

  acquire(&lru.lock);
  n = lru.count;
  release(&lru.lock);

  return n;
}

void
lru_add(pagetable_t pt, uint64 va, uint64 pa)
{
  if(pa < KERNBASE || pa >= PHYSTOP)
    panic("lru_add: bad pa");

  struct page *pg = &pages[(pa-KERNBASE)/PGSIZE];

  acquire(&lru.lock);

  if(pg->used == 0)
    lru.count++;

  pg->pagetable = pt;
  pg->vaddr = PGROUNDDOWN(va);
  pg->age = 0;
  pg->used = 1;
  pg->next = 0;
  pg->prev = 0;

  release(&lru.lock);
}

void
lru_remove(uint64 pa)
{
  if(pa < KERNBASE || pa >= PHYSTOP)
    panic("lru_remove: bad pa");

  struct page *pg = &pages[(pa-KERNBASE)/PGSIZE];

  acquire(&lru.lock);

  if(pg->used){
    clear_page_meta(pg);
    if(lru.count > 0)
      lru.count--;
  }

  release(&lru.lock);
}

void
aging_update(void)
{
  int need_fence = 0;

  acquire(&lru.lock);

  for(int i=0;i<(PHYSTOP-KERNBASE)/PGSIZE;i++){
    struct page *pg = &pages[i];

    if(pg->used == 0 || pg->pagetable == 0)
      continue;

    pte_t *pte = walk(pg->pagetable, pg->vaddr, 0);

    if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0 || (*pte & PTE_S)){
      clear_page_meta(pg);
      if(lru.count > 0)
        lru.count--;
      continue;
    }

    if(PTE2PA(*pte) != page_to_pa(pg)){
      clear_page_meta(pg);
      if(lru.count > 0)
        lru.count--;
      continue;
    }

    pg->age >>= 1;

    if(*pte & PTE_A){
      pg->age |= 0x80;
      *pte &= ~PTE_A;
      need_fence = 1;
    }
  }

  release(&lru.lock);

  if(need_fence)
    sfence_vma();
}

uint64
lru_select_victim(pagetable_t *out_pt, uint64 *out_va)
{
  uchar initCand = 0xff;
  struct page *cand = 0;
  pte_t *cand_pte = 0;

  acquire(&lru.lock);

  for(int i=0;i<(PHYSTOP-KERNBASE)/PGSIZE;i++){
    struct page *pg = &pages[i];
    if(pg->used == 0)
      continue;

    if(pg->pagetable == 0)
      continue;

    pte_t *pte = walk(pg->pagetable, pg->vaddr, 0);

    if(pte == 0 || (*pte & PTE_U) == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_S)){
      clear_page_meta(pg);
      if(lru.count > 0)
        lru.count--;
      continue;
    }

    if(PTE2PA(*pte) != page_to_pa(pg)){
      clear_page_meta(pg);
      if(lru.count > 0)
        lru.count--;
      continue;
    }

    if(cand == 0 || pg->age < initCand){
      initCand = pg->age;
      cand = pg;
      cand_pte = pte;
    }
  }

  if(cand == 0 || cand_pte == 0){
    release(&lru.lock);
    return 0;
  }

  uint64 pa = PTE2PA(*cand_pte);

  if(out_pt)
    *out_pt = cand->pagetable;
  if(out_va)
    *out_va = cand->vaddr;

  clear_page_meta(cand);
  if(lru.count > 0)
    lru.count--;

  release(&lru.lock);

  return pa;
}
