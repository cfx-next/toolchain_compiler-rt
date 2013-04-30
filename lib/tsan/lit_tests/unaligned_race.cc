// RUN: %clangxx_tsan -O1 %s -o %t && %t 2>&1 | FileCheck %s
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

uint64_t objs[8*2*(2 + 4 + 8)][2];

extern "C" {
uint16_t __sanitizer_unaligned_load16(void *addr);
uint32_t __sanitizer_unaligned_load32(void *addr);
uint64_t __sanitizer_unaligned_load64(void *addr);
void __sanitizer_unaligned_store16(void *addr, uint16_t v);
void __sanitizer_unaligned_store32(void *addr, uint32_t v);
void __sanitizer_unaligned_store64(void *addr, uint64_t v);
}

// All this mess is to generate unique stack for each race,
// otherwise tsan will suppress similar stacks.

static void access(char *p, int sz, int rw) {
  if (rw) {
    switch (sz) {
    case 0: __sanitizer_unaligned_store16(p, 0); break;
    case 1: __sanitizer_unaligned_store32(p, 0); break;
    case 2: __sanitizer_unaligned_store64(p, 0); break;
    default: exit(1);
    }
  } else {
    switch (sz) {
    case 0: __sanitizer_unaligned_load16(p); break;
    case 1: __sanitizer_unaligned_load32(p); break;
    case 2: __sanitizer_unaligned_load64(p); break;
    default: exit(1);
    }
  }
}

static int accesssize(int sz) {
  switch (sz) {
  case 0: return 2;
  case 1: return 4;
  case 2: return 8;
  }
  exit(1);
}

template<int off, int off2>
static void access3(bool main, int sz1, bool rw, char *p) {
  p += off;
  if (main) {
    access(p, sz1, true);
  } else {
    p += off2;
    if (rw) {
      *p = 42;
    } else {
       if (*p == 42)
         printf("bingo!\n");
    }
  }
}

template<int off>
static void access2(bool main, int sz1, int off2, bool rw, char *obj) {
  if (off2 == 0)
    access3<off, 0>(main, sz1, rw, obj);
  else if (off2 == 1)
    access3<off, 1>(main, sz1, rw, obj);
  else if (off2 == 2)
    access3<off, 2>(main, sz1, rw, obj);
  else if (off2 == 3)
    access3<off, 3>(main, sz1, rw, obj);
  else if (off2 == 4)
    access3<off, 4>(main, sz1, rw, obj);
  else if (off2 == 5)
    access3<off, 5>(main, sz1, rw, obj);
  else if (off2 == 6)
    access3<off, 6>(main, sz1, rw, obj);
  else if (off2 == 7)
    access3<off, 7>(main, sz1, rw, obj);
}

static void access1(bool main, int off, int sz1, int off2, bool rw, char *obj) {
  if (off == 0)
    access2<0>(main, sz1, off2, rw, obj);
  else if (off == 1)
    access2<1>(main, sz1, off2, rw, obj);
  else if (off == 2)
    access2<2>(main, sz1, off2, rw, obj);
  else if (off == 3)
    access2<3>(main, sz1, off2, rw, obj);
  else if (off == 4)
    access2<4>(main, sz1, off2, rw, obj);
  else if (off == 5)
    access2<5>(main, sz1, off2, rw, obj);
  else if (off == 6)
    access2<6>(main, sz1, off2, rw, obj);
  else if (off == 7)
    access2<7>(main, sz1, off2, rw, obj);
}

void Test(bool main) {
  uint64_t *obj = objs[0];
  for (int off = 0; off < 8; off++) {
    for (int sz1 = 0; sz1 < 3; sz1++) {
      for (int off2 = 0; off2 < accesssize(sz1); off2++) {
        for (int rw = 0; rw < 2; rw++) {
          printf("thr=%d off=%d sz1=%d off2=%d rw=%d p=%p\n",
                 main, off, sz1, off2, rw, obj);
          access1(main, off, sz1, off2, rw, (char*)obj);
          obj += 2;
        }
      }
    }
  }
}

void *Thread(void *p) {
  (void)p;
  sleep(1);
  Test(false);
  return 0;
}

int main() {
  pthread_t th;
  pthread_create(&th, 0, Thread, 0);
  Test(true);
  pthread_join(th, 0);
  printf("OK\n");
}

// WARNING: ThreadSanitizer: data race
// CHECK: ThreadSanitizer: reported 224 warnings
// CHECK: OK