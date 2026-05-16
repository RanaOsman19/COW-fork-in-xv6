#include "kernel/fcntl.h"
#include "kernel/fs.h"
#include "kernel/memlayout.h"
#include "kernel/param.h"
#include "kernel/riscv.h"
#include "kernel/stat.h"
#include "kernel/types.h"
#include "user/user.h"

// PTE flag bits same values as kernel/riscv.h
#define F_W (1 << 2)
#define F_COW (1 << 8)

// use sbrk() to count how many free physical memory pages there are.
int countfree() {
  int n = 0;
  uint64 sz0 = (uint64)sbrk(0);
  while (1) {
    char *a = sbrk(PGSIZE);
    if (a == (char *)-1) {
      break;
    }
    n += 1;
  }
  sbrk(-((uint64)sbrk(0) - sz0));
  return n;
}


// basic COW: fork, child writes, parent should still see original.

void cowbasic(char *s) {
  int *p = (int *)sbrk(4096);
  if (p == (int *)-1) {
    printf("%s: sbrk failed\n", s);
    exit(1);
  }
  *p = 42;

  int pid = fork();
  if (pid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  }
  if (pid == 0) {
    // child writes triggers store page fault scause 15 which calls handle_cow_fault in vm.c
    *p = 99;
    if (*p != 99) {
      printf("%s: child value wrong\n", s);
      exit(1);
    }
    exit(0);
  }
  int xstatus;
  wait(&xstatus);
  if (xstatus != 0) {
    printf("%s: child failed\n", s);
    exit(1);
  }
  if (*p != 42) {
    printf("%s: parent value changed to %d\n", s, *p);
    exit(1);
  }
}


// 3 children sharing the same physical page.
void threechildren(char *s) {
  int *p = (int *)sbrk(4096);
  *p = 10;
  int i;

  for (i = 0; i < 3; i++) {
    int pid = fork();
    if (pid < 0) {
      printf("%s: fork failed\n", s);
      exit(1);
    }
    if (pid == 0) {
      // each child writes a different value
      *p = 100 + i;
      if (*p != 100 + i) {
        printf("%s: child %d got wrong value\n", s, i);
        exit(1);
      }
      exit(0);
    }
  }

  for (i = 0; i < 3; i++) {
    int xstatus;
    wait(&xstatus);
    if (xstatus != 0) {
      printf("%s: child failed\n", s);
      exit(1);
    }
  }

  if (*p != 10) {
    printf("%s: parent value changed to %d\n", s, *p);
    exit(1);
  }
}


// parent writes after fork.

void parentwrite(char *s) {
  int *p = (int *)sbrk(4096);
  *p = 55;

  int pid = fork();
  if (pid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  }

  if (pid == 0) {
    // child waits, then checks its value
    pause(5);
    if (*p != 55) {
      printf("%s: child saw %d, expected 55\n", s, *p);
      exit(1);
    }
    exit(0);
  }

  // parent writes right away
  *p = 77;

  int xstatus;
  wait(&xstatus);
  if (xstatus != 0) {
    printf("%s: child failed\n", s);
    exit(1);
  }
  if (*p != 77) {
    printf("%s: parent value is %d, expected 77\n", s, *p);
    exit(1);
  }
}


// both parent and child write to the same COW page.

void bothwrite(char *s) {
  int *p = (int *)sbrk(4096);
  *p = 1;

  int pid = fork();
  if (pid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  }

  if (pid == 0) {
    *p = 2;
    if (*p != 2) {
      printf("%s: child got %d\n", s, *p);
      exit(1);
    }
    exit(0);
  }

  wait(0);

  // parent writes after child is done
  *p = 3;
  if (*p != 3) {
    printf("%s: parent got %d, expected 3\n", s, *p);
    exit(1);
  }
}


// new pages from sbrk() after fork should have PTE_W and no PTE_COW.

void sbrknocow(char *s) {
  int pid = fork();
  if (pid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  }

  if (pid == 0) {
    // child allocates new memory after fork
    int *q = (int *)sbrk(4096);
    if (q == (int *)-1) {
      printf("%s: sbrk in child failed\n", s);
      exit(1);
    }
    *q = 123;

    // check PTE flags using getflags syscall
    int fl = getflags((uint64)q);
    if (!(fl & F_W)) {
      printf("%s: sbrk page missing PTE_W\n", s);
      exit(1);
    }
    if (fl & F_COW) {
      printf("%s: sbrk page has PTE_COW set\n", s);
      exit(1);
    }
    exit(0);
  }

  int xstatus;
  wait(&xstatus);
  if (xstatus != 0) {
    printf("%s: child failed\n", s);
    exit(1);
  }
}


// text segment should NOT get COW flag after fork.

void textnocow(char *s) {
  int pid = fork();
  if (pid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  }

  if (pid == 0) {
    int fl = getflags((uint64)textnocow);
    if (fl & F_COW) {
      printf("%s: text page has COW flag\n", s);
      exit(1);
    }
    exit(0);
  }

  int xstatus;
  wait(&xstatus);
  if (xstatus != 0) {
    printf("%s: child failed\n", s);
    exit(1);
  }
}

// test copyout with COW pages.

void cowcopyout(char *s) {
  char *buf = sbrk(4096);
  memset(buf, 'A', 4096);

  int fds[2];
  if (pipe(fds) < 0) {
    printf("%s: pipe failed\n", s);
    exit(1);
  }

  int pid = fork();
  if (pid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  }

  if (pid == 0) {
    close(fds[1]);
    int n = read(fds[0], buf, 5);
    close(fds[0]);
    if (n != 5) {
      printf("%s: read returned %d, expected 5\n", s, n);
      exit(1);
    }
    if (buf[0] != 'h' || buf[1] != 'e') {
      printf("%s: wrong data in buf\n", s);
      exit(1);
    }
    exit(0);
  }

  close(fds[0]);
  write(fds[1], "hello", 5);
  close(fds[1]);

  int xstatus;
  wait(&xstatus);
  if (xstatus != 0) {
    printf("%s: child failed\n", s);
    exit(1);
  }
}

// stress test: fork 20 children, each writes to a COW page.

void cowstress(char *s) {
  int *p = (int *)sbrk(4096);
  *p = 0;
  int n = 20;
  int i, count = 0;

  for (i = 0; i < n; i++) {
    int pid = fork();
    if (pid < 0) {
      break;
    }
    if (pid == 0) {
      *p = i;
      exit(0);
    }
    count++;
  }

  for (i = 0; i < count; i++)
    wait(0);

  if (*p != 0) {
    printf("%s: parent value changed to %d\n", s, *p);
    exit(1);
  }
}

// memory leak test: fork+exit 50 times in a loop.

void cowleak(char *s) {
  int *p = (int *)sbrk(4096);
  *p = 7;
  int i;

  int free0 = countfree();

  for (i = 0; i < 50; i++) {
    int pid = fork();
    if (pid < 0) {
      printf("%s: fork failed at iteration %d\n", s, i);
      exit(1);
    }
    if (pid == 0) {
      exit(0);
    }
    wait(0);
  }

  int free1 = countfree();
  if (free0 - free1 > 2) {
    printf("%s: lost %d pages\n", s, free0 - free1);
    exit(1);
  }
}


// performance: COW fork should be fast because it doesn't copy pages.
void cowspeed(char *s) {
  int npg = 40;
  char *mem = sbrk(npg * 4096);
  if (mem == (char *)-1) {
    printf("%s: sbrk failed\n", s);
    exit(1);
  }
  int i;
  for (i = 0; i < npg; i++)
    mem[i * 4096] = (char)i;

  int t0 = uptime();
  int pid = fork();
  int t1 = uptime();

  if (pid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  }
  if (pid == 0) {
    exit(0);
  }
  wait(0);

  int dt = t1 - t0;
  printf("  fork with %d pages: %d ticks\n", npg, dt);
  if (dt > 3) {
    printf("%s: fork took %d ticks, too slow\n", s, dt);
    exit(1);
  }
}


// kill a child process while it is writing to COW pages.

void cowkill(char *s) {
  int *p = (int *)sbrk(4096);
  *p = 333;

  int pid = fork();
  if (pid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  }

  if (pid == 0) {
    for (;;)
      *p = *p + 1;
  }

  pause(2);
  kill(pid);
  wait(0);


  if (*p != 333) {
    printf("%s: parent value changed to %d\n", s, *p);
    exit(1);
  }
}


// exec() after fork should release COW page references.

void cowexec(char *s) {
  int *p = (int *)sbrk(4096);
  *p = 500;

  int pid = fork();
  if (pid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  }

  if (pid == 0) {
    char *argv[] = {"echo", "exec_ok", 0};
    exec("echo", argv);
    printf("%s: exec failed\n", s);
    exit(1);
  }

  int xstatus;
  wait(&xstatus);
  if (xstatus != 0) {
    printf("%s: child failed\n", s);
    exit(1);
  }
  if (*p != 500) {
    printf("%s: parent value changed to %d\n", s, *p);
    exit(1);
  }
}


// test table and runner

struct test {
  void (*f)(char *);
  char *s;
};

struct test cowtests[] = {
    {cowbasic, "cowbasic"},
    {threechildren, "threechildren"},
    {parentwrite, "parentwrite"},
    {bothwrite, "bothwrite"},
    {sbrknocow, "sbrknocow"},
    {textnocow, "textnocow"},
    {cowcopyout, "cowcopyout"},
    {cowstress, "cowstress"},
    {cowleak, "cowleak"},
    {cowspeed, "cowspeed"},
    {cowkill, "cowkill"},
    {cowexec, "cowexec"},
    {0, 0},
};

// run each test in its own process. 
int run(void f(char *), char *s) {
  int pid;
  int xstatus;

  printf("test %s: ", s);
  if ((pid = fork()) < 0) {
    printf("runtest: fork error\n");
    exit(1);
  }
  if (pid == 0) {
    f(s);
    exit(0);
  } else {
    wait(&xstatus);
    if (xstatus != 0)
      printf("FAILED\n");
    else
      printf("OK\n");
    return xstatus == 0;
  }
}

int main(int argc, char *argv[]) {
  char *justone = 0;

  if (argc == 2) {
    justone = argv[1];
  }

  printf("cowalltest starting\n");
  int free0 = countfree();

  int fail = 0;
  for (struct test *t = cowtests; t->s != 0; t++) {
    if (justone == 0 || strcmp(t->s, justone) == 0) {
      if (!run(t->f, t->s)) {
        fail = 1;
      }
    }
  }

  int free1 = countfree();
  if (free1 < free0) {
    printf("FAILED! lost some free pages %d (out of %d)\n", free1, free0);
    fail = 1;
  }

  if (fail) {
    printf("SOME TESTS FAILED\n");
    exit(1);
  }

  printf("ALL COW TESTS PASSED\n");
  exit(0);
}
