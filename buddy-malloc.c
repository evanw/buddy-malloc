/*
 * This file implements a buddy memory allocator, which is an allocator that
 * allocates memory within a fixed linear address range. It spans the address
 * range with a binary tree that tracks free space. Both "malloc" and "free"
 * are O(log N) time where N is the maximum possible number of allocations.
 *
 * The "buddy" term comes from how the tree is used. When memory is allocated,
 * nodes in the tree are split recursively until a node of the appropriate size
 * is reached. Every split results in two child nodes, each of which is the
 * buddy of the other. When a node is freed, the node and its buddy can be
 * merged again if the buddy is also free. This makes the memory available
 * for larger allocations again.
 */

#include <memory.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

/*
 * Every allocation needs an 8-byte header to store the allocation size while
 * staying 8-byte aligned. The address returned by "malloc" is the address
 * right after this header (i.e. the size occupies the 8 bytes before the
 * returned address).
 */
#define HEADER_SIZE 8

/*
 * The minimum allocation size is 16 bytes because we have an 8-byte header and
 * we need to stay 8-byte aligned.
 */
#define MIN_ALLOC_LOG2 4
#define MIN_ALLOC ((size_t)1 << MIN_ALLOC_LOG2)

/*
 * The maximum allocation size is currently set to 2gb. This is the total size
 * of the heap. It's technically also the maximum allocation size because the
 * heap could consist of a single allocation of this size. But of course real
 * heaps will have multiple allocations, so the real maximum allocation limit
 * is at most 1gb.
 */
#define MAX_ALLOC_LOG2 31
#define MAX_ALLOC ((size_t)1 << MAX_ALLOC_LOG2)

/*
 * Allocations are done in powers of two starting from MIN_ALLOC and ending at
 * MAX_ALLOC inclusive. Each allocation size has a bucket that stores the free
 * list for that allocation size.
 *
 * Given a bucket index, the size of the allocations in that bucket can be
 * found with "(size_t)1 << (MAX_ALLOC_LOG2 - bucket)".
 */
#define BUCKET_COUNT (MAX_ALLOC_LOG2 - MIN_ALLOC_LOG2 + 1)

/*
 * Free lists are stored as circular doubly-linked lists. Every possible
 * allocation size has an associated free list that is threaded through all
 * currently free blocks of that size. That means MIN_ALLOC must be at least
 * "sizeof(list_t)". MIN_ALLOC is currently 16 bytes, so this will be true for
 * both 32-bit and 64-bit.
 */
typedef struct list_t {
  struct list_t *prev, *next;
} list_t;

/*
 * Each bucket corresponds to a certain allocation size and stores a free list
 * for that size. The bucket at index 0 corresponds to an allocation size of
 * MAX_ALLOC (i.e. the whole address space).
 */
static list_t buckets[BUCKET_COUNT];

/*
 * This array represents a linearized binary tree of bits. Every possible
 * allocation larger than MIN_ALLOC has a node in this tree (and therefore a
 * bit in this array).
 *
 * Given the index for a node, lineraized binary trees allow you to traverse to
 * the parent node or the child nodes just by doing simple arithmetic on the
 * index:
 *
 * - Move to parent:         index = (index - 1) / 2;
 * - Move to left child:     index = index * 2 + 1;
 * - Move to right child:    index = index * 2 + 2;
 * - Move to sibling:        index = ((index - 1) ^ 1) + 1;
 *
 * Each node in this tree can be in one of several states:
 *
 * - UNUSED (both children are UNUSED)
 * - SPLIT (one child is UNUSED and the other child isn't)
 * - USED (neither children are UNUSED)
 *
 * These states take two bits to store. However, it turns out we have enough
 * information to distinguish between UNUSED and USED from context, so we only
 * need to store SPLIT or not, which only takes a single bit.
 *
 * Note that we don't need to store any nodes for allocations of size MIN_ALLOC
 * since we only ever care about parent nodes.
 */
static uint8_t node_is_split[(1 << (BUCKET_COUNT - 1)) / 8];

/*
 * This is the starting address of the address range for this allocator. Every
 * returned allocation will be an offset of this pointer from 0 to MAX_ALLOC.
 */
static uint8_t *base_ptr;

/*
 * This is the maximum address that has ever been used by the allocator. It's
 * used to know when to call "brk" to request more memory from the kernel.
 */
static uint8_t *max_ptr;

/*
 * Make sure all addresses before "new_value" are valid and can be used. Memory
 * is allocated in a 2gb address range but that memory is not reserved up
 * front. It's only reserved when it's needed by calling this function. This
 * will return false if the memory could not be reserved.
 */
static int update_max_ptr(uint8_t *new_value) {
  if (new_value > max_ptr) {
    if (brk(new_value)) {
      return 0;
    }
    max_ptr = new_value;
  }
  return 1;
}

/*
 * Initialize a list to empty. Because these are circular lists, an "empty"
 * list is an entry where both links point to itself. This makes insertion
 * and removal simpler because they don't need any branches.
 */
static void list_init(list_t *list) {
  list->prev = list;
  list->next = list;
}

/*
 * Append the provided entry to the end of the list. This assumes the entry
 * isn't in a list already because it overwrites the linked list pointers.
 */
static void list_push(list_t *list, list_t *entry) {
  list_t *prev = list->prev;
  entry->prev = prev;
  entry->next = list;
  prev->next = entry;
  list->prev = entry;
}

/*
 * Remove the provided entry from whichever list it's currently in. This
 * assumes that the entry is in a list. You don't need to provide the list
 * because the lists are circular, so the list's pointers will automatically
 * be updated if the first or last entries are removed.
 */
static void list_remove(list_t *entry) {
  list_t *prev = entry->prev;
  list_t *next = entry->next;
  prev->next = next;
  next->prev = prev;
}

/*
 * Remove and return the first entry in the list or NULL if the list is empty.
 */
static list_t *list_pop(list_t *list) {
  list_t *back = list->prev;
  if (back == list) return NULL;
  list_remove(back);
  return back;
}

/*
 * This maps from the index of a node to the address of memory that node
 * represents. The bucket can be derived from the index using a loop but is
 * required to be provided here since having them means we can avoid the loop
 * and have this function return in constant time.
 */
static uint8_t *ptr_for_node(size_t index, size_t bucket) {
  return base_ptr + ((index - (1 << bucket) + 1) << (MAX_ALLOC_LOG2 - bucket));
}

/*
 * This maps from an address of memory to the node that represents that
 * address. There are often many nodes that all map to the same address, so
 * the bucket is needed to uniquely identify a node.
 */
static size_t node_for_ptr(uint8_t *ptr, size_t bucket) {
  return ((ptr - base_ptr) >> (MAX_ALLOC_LOG2 - bucket)) + (1 << bucket) - 1;
}

/*
 * Given the index of a node, this flips the "is split" flag of the parent and
 * returns the new value of that flag.
 */
static int flip_parent_is_split(size_t index) {
  index = (index - 1) / 2;
  return node_is_split[index / 8] ^= 1 << (index % 8);
}

/*
 * Given the requested size passed to "malloc", this function returns the index
 * of the smallest bucket that can fit that size.
 */
static size_t bucket_for_request(size_t request) {
  size_t bucket = BUCKET_COUNT - 1;
  size_t size = MIN_ALLOC;

  while (size < request) {
    bucket--;
    size *= 2;
  }

  return bucket;
}

uint8_t *buddy_malloc(size_t request) {
  size_t original_bucket, bucket, i;

  /*
   * Make sure it's possible for an allocation of this size to succeed. There's
   * a hard-coded limit on the maximum allocation size because of the way this
   * allocator works.
   */
  if (request + HEADER_SIZE > MAX_ALLOC) {
    return NULL;
  }

  /*
   * Find the smallest bucket that will fit this request. This doesn't check
   * that there's space for the request yet.
   */
  bucket = bucket_for_request(request + HEADER_SIZE);
  original_bucket = bucket;

  /*
   * Search for a bucket with a non-empty free list that's as large or larger
   * than what we need. If there isn't an exact match, we'll need to split a
   * larger one to get a match.
   */
  while (bucket + 1 != 0) {
    uint8_t *ptr = (uint8_t *)list_pop(&buckets[bucket]);
    size_t size, bytes_needed;

    /*
     * If the free list for this bucket is empty, check the free list for the
     * next largest bucket instead.
     */
    if (!ptr) {
      bucket--;
      continue;
    }

    /*
     * Try to expand the address space first before going any further. If we
     * have run out of space, put this block back on the free list and fail.
     */
    size = (size_t)1 << (MAX_ALLOC_LOG2 - bucket);
    bytes_needed = bucket < original_bucket ? size / 2 + sizeof(list_t) : size;
    if (!update_max_ptr(ptr + bytes_needed)) {
      list_push(&buckets[bucket], (list_t *)ptr);
      return NULL;
    }

    /*
     * If we got a node off the free list, change the node from UNUSED to USED.
     * This involves flipping our parent's "is split" bit because that bit is
     * the exclusive-or of the UNUSED flags of both children, and our UNUSED
     * flag (which isn't ever stored explicitly) has just changed.
     *
     * Note that we shouldn't ever need to flip the "is split" bit of our
     * grandparent because we know our buddy is USED so it's impossible for our
     * grandparent to be UNUSED (if our buddy chunk was UNUSED, our parent
     * wouldn't ever have been split in the first place).
     */
    i = node_for_ptr(ptr, bucket);
    if (i != 0) {
      flip_parent_is_split(i);
    }

    /*
     * If the node we got is larger than we need, split it down to the correct
     * size and put the new unused child nodes on the free list in the
     * corresponding bucket. This is done by repeatedly moving to the left
     * child, splitting the parent, and then adding the right child to the free
     * list.
     */
    while (bucket < original_bucket) {
      i = i * 2 + 1;
      bucket++;
      flip_parent_is_split(i);
      list_push(&buckets[bucket], (list_t *)ptr_for_node(i + 1, bucket));
    }

    /*
     * Now that we have a memory address, write the block header (just the size
     * of the allocation) and return the address immediately after the header.
     */
    *(size_t *)ptr = request;
    return ptr + HEADER_SIZE;
  }

  return NULL;
}

void buddy_free(uint8_t *ptr) {
  size_t bucket, i;

  /*
   * We were given the address returned by "malloc" so get back to the actual
   * address of the node by subtracting off the size of the block header. Then
   * look up the index of the node corresponding to this address.
   */
  ptr -= HEADER_SIZE;
  bucket = bucket_for_request(*(size_t *)ptr + HEADER_SIZE);
  i = node_for_ptr(ptr, bucket);

  /*
   * Traverse up to the root node, flipping USED blocks to UNUSED and merging
   * UNUSED buddies together into a single UNUSED parent.
   */
  while (i != 0) {
    /*
     * Change this node from UNUSED to USED. This involves flipping our
     * parent's "is split" bit because that bit is the exclusive-or of the
     * UNUSED flags of both children, and our UNUSED flag (which isn't ever
     * stored explicitly) has just changed.
     *
     * If the parent is now SPLIT, that means our buddy is USED, so don't merge
     * with it. Instead, stop the iteration here and add ourselves to the free
     * list for our bucket.
     */
    if (flip_parent_is_split(i)) {
      break;
    }

    /*
     * If we get here, we know our buddy is UNUSED. In this case we should
     * merge with that buddy and continue traversing up to the root node. We
     * need to remove the buddy from its free list here but we don't need to
     * add the merged parent to its free list yet. That will be done once after
     * this loop is finished.
     */
    list_remove((list_t *)ptr_for_node(((i - 1) ^ 1) + 1, bucket));
    i = (i - 1) / 2;
    bucket--;
  }

  /*
   * Add ourselves to the free list for our bucket. We add to the back of the
   * list because "malloc" takes from the back of the list and we want a "free"
   * followed by a "malloc" of the same size to ideally use the same address
   * for better memory locality.
   */
  list_push(&buckets[bucket], (list_t *)ptr_for_node(i, bucket));
}

/*
 * Initialize the state of the allocator. This must be called before any
 * allocations happen. Note that this relies on static global memory being
 * zero-initialized by the compiler.
 */
void buddy_init() {
  size_t i;

  /*
   * At the beginning, most of our memory is unreserved. We only need to
   * reserve space for a single free list entry for the root node, which
   * represents the entire address space.
   */
  base_ptr = max_ptr = sbrk(0);
  update_max_ptr(base_ptr + sizeof(list_t));

  /*
   * Free lists are circular linked lists, so each list defaults to pointing to
   * itself instead of pointing to null. These pointers are initialized here.
   */
  for (i = 0; i < BUCKET_COUNT; i++) {
    list_init(&buckets[i]);
  }

  /*
   * Push a single block onto the free list for the largest bucket. This block
   * represents the entire address space.
   */
  list_push(&buckets[0], (list_t *)base_ptr);
}
