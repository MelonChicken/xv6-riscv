// user/mmap_basic_test.c

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define MMAPBASE 0x40000000L
#define PGSIZE   4096

#define PROT_READ  0x1
#define PROT_WRITE 0x2

#define MAP_ANONYMOUS 0x1
#define MAP_POPULATE  0x2

int
main(void)
{
  uint64 addr;

  printf("mmap basic test start\n");

  addr = mmap(0, PGSIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS, -1, 0);

  if(addr == MMAPBASE){
    printf("PASS: mmap returned expected address  %ld\n", addr);
  } else {
    printf("FAIL: mmap returned %ld, expected %ld\n", addr, MMAPBASE);
  }

  exit(0);
}