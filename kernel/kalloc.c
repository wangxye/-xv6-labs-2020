// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);
void actual_freerange(void *pa_start, void *pa_end, int idx);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];

void
kinit()
{
  uint64 avg, p;
  for(int i = 0; i < NCPU; i++) {
    initlock(&kmem[i].lock, "kmem");
  }

  int idx = 0;
  avg = (PHYSTOP - (uint64)end) / NCPU;
  for(p = (uint64)end; p <= PHYSTOP; p+=avg) {
    actual_freerange((void *)p, p + avg < PHYSTOP?(void *)(p + avg):(void*)PHYSTOP, idx);
    idx++;
  }

  //initlock(&kmem.lock, "kmem");
  //freerange(end, (void*)PHYSTOP);
}


void
actual_freerange(void *pa_start, void *pa_end, int i)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    actual_kfree(p, i);
}



void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

void
actual_kfree(void *pa, int idx)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem[idx].lock);
  r->next = kmem[idx].freelist;
  kmem[idx].freelist = r;
  release(&kmem[idx].lock);
}



// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  //struct run *r;
  int idx = 0;
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");


  push_off();
  idx = cpuid();
  pop_off();
  actual_kfree(pa, idx);

  // Fill with junk to catch dangling refs.
  //memset(pa, 1, PGSIZE);


  //r = (struct run*)pa;

  //acquire(&kmem.lock);
  //r->next = kmem.freelist;
  //kmem.freelist = r;
  //release(&kmem.lock);
}

void * actual_kalloc(int idx) {
  struct run *r;

  acquire(&kmem[idx].lock);
  r = kmem[idx].freelist;
  if(r)
    kmem[idx].freelist = r->next;
  release(&kmem[idx].lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  //struct run *r;
  int idx = 0;
  void * pa;
  push_off();
  idx = cpuid();
  pop_off();
  
  pa = actual_kalloc(idx);

  if (pa) return pa;

  for(int i = 0; i < NCPU; i++) {
    pa = actual_kalloc(i);
    if(pa) return pa;
  }

  return 0;

  /*
  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
  */
}
