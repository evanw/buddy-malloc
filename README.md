# Buddy Memory Allocator

*This allocator hasn't been benchmarked at all and has barely been tested. Use at your own risk!*

The file [buddy-malloc.c](./buddy-malloc.c) implements a buddy memory allocator, which is an allocator that allocates memory within a fixed linear address range. It spans the address range with a binary tree that tracks free space. Both "malloc" and "free" are O(log N) time where N is the maximum possible number of allocations.

The "buddy" term comes from how the tree is used. When memory is allocated, nodes in the tree are split recursively until a node of the appropriate size is reached. Every split results in two child nodes, each of which is the buddy of the other. When a node is freed, the node and its buddy can be merged if the buddy is also free. This makes the memory available for larger allocations again. [Wikipedia](https://en.wikipedia.org/wiki/Buddy_memory_allocation) has more information.

I wrote this because I needed a simple allocator for a side project (a compiler for WebAssembly) and I didn't feel like attempting to port a standard memory allocator (they are many thousands of lines long and are really complicated). I found a few other smaller allocators but ended up not being able to use them. For example, one of the simplest allocators is the one from K&R C book, but it's O(N) time which is unacceptably slow.

The code uses the Linux kernel as inspiration in a few places. One is the use of [circular doubly-linked lists](https://github.com/torvalds/linux/blob/master/include/linux/list.h) to track free memory blocks. Another trick is using a single bit per node to store the state of the node, which is described in detail [here](https://www.kernel.org/doc/gorman/html/understand/understand009.html). This allocator uses the "brk" syscall to request more memory from the kernel. While archaic, this method is a good fit for WebAssembly's linear memory model.

This code is available under the [MIT license](./LICENSE.md).
