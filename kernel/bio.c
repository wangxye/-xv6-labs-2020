// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

struct {
  struct spinlock lock;
  struct buf buf[NBUF];
  struct spinlock buf_lock[NBUCKET];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head[NBUCKET];
} bcache;


int hash(int idx) {
  return idx % NBUCKET;
}

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");

  for(int i = 0; i < NBUCKET; i++) {
    initlock(&bcache.buf_lock[i], "bcache_buf_lock");
  }

  // Create linked list of buffers

  for(int i = 0; i < NBUCKET; i++) {
    bcache.head[i].prev = &bcache.head[i];
    bcache.head[i].next = &bcache.head[i];
  
  }

  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.head[0].next;
    b->prev = &bcache.head[0];
    initsleeplock(&b->lock, "buffer");
    bcache.head[0].next->prev = b;
    bcache.head[0].next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b, *b2 = 0;

  int idx = hash(blockno),min_ticks = 0;

  acquire(&bcache.buf_lock[idx]);

  // Is the block already cached?
  for(b = bcache.head[idx].next; b != &bcache.head[idx]; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.buf_lock[idx]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.buf_lock[idx]);

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  acquire(&bcache.lock);
  acquire(&bcache.buf_lock[idx]);
  for(b = bcache.head[idx].next; b != &bcache.head[idx]; b = b->next){
    if(b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&bcache.buf_lock[idx]);
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
    /**
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
      **/
    }

   for(b = bcache.head[idx].next; b != &bcache.head[idx]; b = b->next){
     if(b->refcnt == 0 && (b2 == 0 || b->lastuse < min_ticks)) {
       min_ticks = b->lastuse;
       b2 = b;
     }
   }

   if(b2) {
     b2->dev = dev;
     b2->blockno = blockno;
     b2->refcnt++;
     b2->valid = 0;
     release(&bcache.buf_lock[idx]);
     release(&bcache.lock);
     acquiresleep(&b2->lock);
     return b2;
   }
  
  for(int j = hash(idx + 1); j != idx; j = hash(j + 1)) {
    acquire(&bcache.buf_lock[j]);
    for(b = bcache.head[j].next; b != &bcache.head[j]; b = b->next){
     if(b->refcnt == 0 && (b2 == 0 || b->lastuse < min_ticks)) {
       min_ticks = b->lastuse;
       b2 = b;
     }

     if(b2) {
       b2->dev = dev;
       b2->blockno = blockno;
       b2->refcnt++;
       b2->valid = 0;

       b2->next->prev = b2->prev;
       b2->prev->next = b2->next;

       release(&bcache.buf_lock[j]);
       b2->next = bcache.head[idx].next;
       b2->prev = &bcache.head[idx];

       bcache.head[idx].next->prev = b2;
       bcache.head[idx].next = b2;

       release(&bcache.buf_lock[idx]);
       release(&bcache.lock);
       acquiresleep(&b2->lock);
       return b2;
     }
   }

    release(&bcache.buf_lock[j]);
  }
   
   
  release(&bcache.buf_lock[idx]);
  release(&bcache.lock);
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);
  
  int i = hash(b->blockno);

  acquire(&bcache.buf_lock[i]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    /*
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next = b;
    */
    b->lastuse = ticks;
  }
  
  release(&bcache.buf_lock[i]);
}

void
bpin(struct buf *b) {
  int i = hash(b->blockno);
  acquire(&bcache.buf_lock[i]);
  b->refcnt++;
  release(&bcache.buf_lock[i]);
}

void
bunpin(struct buf *b) {
  int i = hash(b->blockno);
  acquire(&bcache.buf_lock[i]);
  b->refcnt--;
  release(&bcache.buf_lock[i]);
}


