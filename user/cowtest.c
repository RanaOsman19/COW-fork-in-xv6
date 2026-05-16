#include "kernel/types.h"
#include "user/user.h"
#include "kernel/riscv.h"

int main(){
  printf("Starting COW logic test...\n");
  
  char*p = sbrk(4096);
  uint64 va = (uint64)p;
  
  uint64 flags = getflags(va);
  printf("Initial flags: %ld\n", flags);
  
  if(flags & PTE_W) {
      printf("SUCCESS: Page is writable.\n");
  }
  
  exit(0);
}
