/*
	Buddy system interface.
*/

#ifndef BUDDY_H_
#define BUDDY_H_

#include<stdlib.h>

// Size of a block
#ifndef BLOCK_SIZE
#define BLOCK_SIZE 4096
#endif


// L1 cache line size
#ifndef CACHE_L1_LINE_SIZE
#define CACHE_L1_LINE_SIZE 64
#endif

// Maximum order of two, 128GB limit
#define MAX_ORDER_LIMIT 25

// Size conversions
#define size_in_blocks(size) (size/BLOCK_SIZE + (size%BLOCK_SIZE != 0))
#define size_of_blocks(order) ((1<<order)*BLOCK_SIZE)
#define size_in_bytes(block_count) (BLOCK_SIZE*block_count)
#define size_in_L1(size) (size/CACHE_L1_LINE_SIZE + (size%CACHE_L1_LINE_SIZE != 0))

// Power of two calculations
#define power_of_two(order) (unsigned int)(1<<order)
unsigned int calc_block_order(size_t size);
unsigned int calc_max_order(unsigned int num);

// Size in blocks
typedef unsigned long block_count_t;

// Block index
typedef unsigned long block_index_t;


// Block area hook
typedef struct block_area
{
	void* addr;
	unsigned int order;
}block_area_t;


// Initialize buddy system
int buddy_init(void* mem_space, block_count_t block_count);

// Allocate 2^order blocks
block_area_t buddy_alloc(unsigned int order);

// Free 2^order blocks	
int buddy_free(block_area_t *block_area);   

// Allocate space for kernel control structure
void *kernel_ctrl_alloc(size_t size);


#endif //BUDDY_H_