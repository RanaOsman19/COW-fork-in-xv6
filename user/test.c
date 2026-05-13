#include "kernel/types.h"
#include "user/user.h"

int main() {
    printf("=== COW Test ===\n");
    
    // Allocate a page
    int *x = (int *)sbrk(4096);
    *x = 100;
    printf("Parent set x = %d at address %p\n", *x, x);
    
    int pid = fork();
    
    if(pid == 0) {
        // Child process
        printf("Child reads x = %d\n", *x);
        *x = 200;
        printf("Child wrote x = %d\n", *x);
        exit(0);
    } else {
        // Parent process
        wait(0);
        printf("Parent reads x = %d\n", *x);
        
        if(*x == 100) {
            printf("SUCCESS: COW fork works!\n");
        } else {
            printf("FAIL: Parent value changed to %d\n", *x);
        }
        exit(0);
    }
}