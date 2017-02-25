/*
	Buddy system implementation
*/

#include "buddy.h"





/*
	Type definitions
*/

// Null index
#define NULL_INDEX 0

// First index for allocation
#define FIRST_ALLOC_INDEX 1

// Block pointer
typedef void* block_t;

// Buddy control structure
typedef struct buddy_struct
{
	void* alloc_space;
	block_count_t alloc_block_count;
	block_count_t free_block_count;
	block_index_t free_heads[MAX_ORDER_LIMIT];
	unsigned int max_order;
	unsigned int ctrl_offset;
}buddy_struct_t;





/* 
	Block manipulation macros
*/

#define get_block(block_index) (block_t)((char*)(mem_space) + BLOCK_SIZE*block_index)
#define get_index(block_ptr) (block_index_t)((char*)block_ptr - (char*)mem_space)/BLOCK_SIZE;
#define get_next_index(block_ptr) (block_index_t)(*(block_index_t*)block_ptr)
#define set_next_index(cur_block_ptr, next_block_index) (block_index_t)(*(block_index_t*)cur_block_ptr) = next_block_index
#define null_next_index(block_ptr)  (block_index_t)(*(block_index_t*)block_ptr) = NULL_INDEX




/*
	Global variables
*/

// Memory space location
void* mem_space;

// Control structure pointer
buddy_struct_t *buddy_ctrl_struct;





/*
	Calculations
*/

// Calculates maximum order of two found in num
unsigned int calc_max_order(unsigned int num)
{
	int m = -1;
	while (num)
	{
		m++;
		num >>= 1;
	}
	return m;
}

// Calculates number of blocks needed for size (order of two)
unsigned int calc_block_order(size_t size)
{
	unsigned int order = 0;

	if (size > BLOCK_SIZE)
	{
		order = 1;
		while (power_of_two(order) < size_in_blocks(size))
			order++;
	}

	return order;
}

// Calculates index of buddy block
block_index_t calc_buddy_index(block_index_t block_index, unsigned int order)
{
	char left, sign;
	if (block_index % power_of_two(order) != 1 && order != 0)
		return NULL_INDEX;

	left = (block_index % power_of_two(order + 1)) == 1;
	sign = -1 * (!left) + left;
	return block_index + sign*power_of_two(order);
}






/*
	List operations
*/

void put_first(block_index_t block_index, unsigned int order)
{
	block_index_t head_index;

	head_index = buddy_ctrl_struct->free_heads[order]; 
	set_next_index(get_block(block_index), head_index);
	buddy_ctrl_struct->free_heads[order] = block_index;
}


block_index_t remove(block_index_t block_index, unsigned int order)
{
	block_index_t cur,prev;
	unsigned int max_order = buddy_ctrl_struct->max_order;
	cur = buddy_ctrl_struct->free_heads[order];
	prev = NULL_INDEX;
	
	if (block_index == NULL_INDEX || block_index > buddy_ctrl_struct->alloc_block_count || order > max_order || cur == NULL_INDEX)
		return NULL_INDEX;

	
	while (cur != block_index)
	{
		prev = cur;
		cur = get_next_index(get_block(cur));

		if (cur == NULL_INDEX)
			return NULL_INDEX;
	}

	if(prev != NULL_INDEX)
	{
		set_next_index(get_block(prev), get_next_index(get_block(cur)));
	}
	else
	{
		buddy_ctrl_struct->free_heads[order] = get_next_index(get_block(cur));
	}

	null_next_index(get_block(cur));
	return cur;
}





/*
	Buddy allocator functions	
*/

// Initialize buddy allocator
int buddy_init(void* space, block_count_t block_count)
{

	int order;
	block_index_t block_index = FIRST_ALLOC_INDEX;

	mem_space = space;

	buddy_ctrl_struct = (buddy_struct_t*)mem_space;

	block_count--;
	order = calc_max_order(block_count);
	buddy_ctrl_struct->max_order = (unsigned int)order;

	buddy_ctrl_struct->alloc_block_count = block_count;
	buddy_ctrl_struct->alloc_space = (void*)((char*)mem_space + BLOCK_SIZE*FIRST_ALLOC_INDEX);

	buddy_ctrl_struct->ctrl_offset = size_in_L1(sizeof(buddy_struct_t))*CACHE_L1_LINE_SIZE;

	buddy_ctrl_struct->free_block_count = block_count;

	while (order>-1)
	{
		if (block_count & power_of_two(order))
		{
			buddy_ctrl_struct->free_heads[order] = block_index;
			null_next_index(get_block(block_index));
			block_index += power_of_two(order);
		}
		else
		{
			buddy_ctrl_struct->free_heads[order] = NULL_INDEX;
		}
		order--;
	}

	return 0;
}


// Allocate blocks
block_area_t buddy_alloc(unsigned int order)
{
	block_index_t head_index,temp_index,buddy_index;
	block_t free_block = NULL;
	block_count_t block_count = power_of_two(order);
	block_area_t ret;
	unsigned int temp_order;


	if (buddy_ctrl_struct->free_block_count >= block_count && order <= buddy_ctrl_struct->max_order)
	{
		head_index = buddy_ctrl_struct->free_heads[order];

		if (head_index != NULL_INDEX)
		{
			temp_index = remove(head_index, order);
		}
		else
		{
			temp_order = order;

			while (buddy_ctrl_struct->free_heads[temp_order] == NULL_INDEX)
				temp_order++;

			head_index = buddy_ctrl_struct->free_heads[temp_order];

			temp_index = remove(head_index, temp_order);


			while (temp_order != order)
			{
				buddy_index = temp_index + power_of_two(temp_order-1);
				
				put_first(buddy_index,temp_order-1);
				temp_order--;
			}
		}

		free_block = get_block(temp_index);
		buddy_ctrl_struct->free_block_count -= block_count;
	}
	else
	{
		free_block = NULL;
		temp_index = NULL_INDEX;
	}
	
	ret.addr = free_block;
	ret.order = order;
	
	return ret;
}


// Free blocks
int buddy_free(block_area_t *block_area)
{

	block_index_t index = get_index(block_area->addr);
	unsigned int order = block_area->order;
	block_index_t buddy_index = calc_buddy_index(index,order);
	block_count_t block_count = power_of_two(order);

	if (index > NULL_INDEX && index <= buddy_ctrl_struct->alloc_block_count)
	{
		buddy_index = remove(buddy_index, order);

		while (buddy_index != NULL_INDEX)
		{
			if (buddy_index < index)
				index = buddy_index;

			order++;

			buddy_index = calc_buddy_index(index, order);
			buddy_index = remove(buddy_index, order);
		}

		put_first(index, order);

		buddy_ctrl_struct->free_block_count += block_count;
	}
	else
	{
		return -1;
	}
	
	return 0;
}


// Allocate kernel control space
void *kernel_ctrl_alloc(size_t size)
{
	void *mem;

	if (buddy_ctrl_struct->ctrl_offset >= BLOCK_SIZE)
		return NULL;

	mem = (void*)((char*)mem_space + buddy_ctrl_struct->ctrl_offset);
	buddy_ctrl_struct->ctrl_offset += size_in_L1(size)*CACHE_L1_LINE_SIZE;

	return mem;
}