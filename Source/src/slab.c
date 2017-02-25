/*
	Slab allocator implementation
*/

#include "slab.h"
#include "buddy.h"
#include "mutex.h"
#include <memory.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>


/*
	Allocator parameters
*/

// Cache name length
#define CACHE_NAME_LEN 32

// Size-N buffer constants
#define MIN_BUFF_ORDER 5
#define MAX_BUFF_ORDER 17
#define SIZE_N_COUNT (MAX_BUFF_ORDER - MIN_BUFF_ORDER + 1)

// Minimum number of objects per slab
#define MIN_OBJ_CNT 1

// Call constructor when object is set free
//#define FREE_CTOR

// Call destructor when object is set free
//#define FREE_DTOR




/*
	Type definitions
*/

// Index type
typedef unsigned int index_t;

// Slab type type
typedef enum slab_type {empty, partial, full} slab_type_t;





/*
	Pointer manipulations
*/

// Pointer offset calculation
#define ptr_offset(ptr,offset) (void*)((char*)ptr + offset)



/*
	Bitmaps
*/

//Bitmap entry
typedef unsigned char bitmap_entry_t;

// Slab bitmap macros
#define BITMAP_EMPTY 0x00
#define BITMAP_FULL 0xff
#define BITMAP_ENTRY_BITS 8
#define bitmap_set_used(bitmap, index) bitmap[index/BITMAP_ENTRY_BITS] |= (1<<(index%BITMAP_ENTRY_BITS))
#define bitmap_set_free(bitmap, index) bitmap[index/BITMAP_ENTRY_BITS] &= ~(1<<(index%BITMAP_ENTRY_BITS))
#define obj_per_entry (sizeof(bitmap_entry_t)*BITMAP_ENTRY_BITS)
#define calc_bitmap_size(obj_count) ((obj_count / obj_per_entry + (obj_count%obj_per_entry!=0)) * sizeof(bitmap_entry_t))





/*
	Error handling
*/

// Error codes
typedef enum error_code {
	err_arg=1, 
	err_malloc, 
	err_free, 
	err_cache_expand, 
	err_cache_create,
	err_cache_obj_alloc,
	err_cache_obj_free,
	err_buff_alloc,
	err_buff_free
} error_code_t;

// Error messages
const char *error_text[] = {
	"Invalid function parameters!",
	"Memmory allocation failed!",
	"Memmory deallocation failed!",
	"Cache expansion failed!",
	"Cache creation failed!",
	"Object allocation failed!",
	"Object dealloaction failed!",
	"Buffer allocation failed!",
	"Buffer dealloaction failed!"
};

// Print error message
void print_error(error_code_t code)
{
	printf_s("Error: %s\n", error_text[code-1]);
}

// Validate expression
#define val_exp(expression) if(!expression) assert(expression)

// Check arguments for functions with pointer return value
#define arg_check_null(arg_exp) if(!arg_exp) { print_error(err_arg); return 0; }

// Check function arguments for void functions
#define arg_check(arg_exp) if(!arg_exp) { print_error(err_arg); return; }

// Check function return value
#define ret_check_null(ret,error_code, mutex) if(ret == NULL) { print_error(error_code); signal(mutex); return 0; }




/*
	Allocator structure definitions
*/

// Slab structure
typedef struct slab
{
	kmem_cache_t *cache;
	slab_type_t type;

	block_area_t my_hook;
	index_t index;

	unsigned int used_count;
	unsigned int offset;

	bitmap_entry_t *bitmap;

	void *objects;

	struct slab *next;

}slab_t;


// Cache structure
typedef struct kmem_cache_s
{
	char name[CACHE_NAME_LEN];

	slab_t *heads[3];

	size_t object_size;
	unsigned int bitmap_length;

	unsigned int slab_order;
	unsigned int slab_count[3];
	
	index_t next_offset;

	unsigned int obj_per_slab;
	unsigned int max_alignments;

	void(*ctor)(void *);
	void(*dtor)(void *);

	char extended;
	
	error_code_t error;

	char mutex_space[MUTEX_SIZE];
	mutex_t mutex;

	struct kmem_cache_s *next;

	//mutex
}kmem_cache_t;


// Small memory buffer
typedef struct kmem_buff_s
{
	kmem_cache_t cache;
	unsigned char used;

}kmem_buff_t;


// Cache control structure
typedef struct kmem_ctrl_s
{
	kmem_cache_t cache;
	kmem_buff_t buffers[SIZE_N_COUNT];

}kmem_ctrl_t;




/*
	Global variables
*/

// Allocator control structure
kmem_ctrl_t *kmem_ctrl;

// Shared mutex
mutex_t sem;





/*
	Interface for buddy system
*/

// Mutex for buddy allocator
mutex_t buddy_sem;


// Allocate blocks
block_area_t block_alloc(unsigned int order)
{
	wait(buddy_sem);

	block_area_t hook = buddy_alloc(order);
	
	
	if (hook.addr == NULL)
	{
		print_error(err_malloc);
	}

	signal(buddy_sem);

	return hook;
}


// Free blocks
void mem_free(block_area_t area)
{
	wait(buddy_sem);

	if (buddy_free(&area) != 0)
		print_error(err_free);

	signal(buddy_sem);
}





/*
	Slab implementation
*/

// Calculates slab order so minimum one object fits
unsigned int calc_slab_order(size_t obj_size)
{
	return calc_block_order(sizeof(slab_t) + obj_size*MIN_OBJ_CNT + sizeof(bitmap_entry_t));
}


// Allocate memmory and initialize a slab
slab_t *slab_alloc(kmem_cache_t *cache, index_t index)
{
	val_exp(cache != NULL);

	block_area_t hook;
	slab_t *slab;
	unsigned int offset;
	void(*ctor)(void*) = cache->ctor;
	int i;

	hook = block_alloc(cache->slab_order);

	if (hook.addr == NULL)
		return NULL;

	offset = (index % (cache->max_alignments)) * CACHE_L1_LINE_SIZE;

	slab = (slab_t*)ptr_offset(hook.addr, offset);

	slab->cache = cache;
	slab->my_hook = hook;
	slab->next = NULL;
	slab->offset = offset;
	slab->used_count = 0;
	slab->bitmap = (bitmap_entry_t*)ptr_offset(slab,sizeof(slab_t));
	slab->objects = ptr_offset(slab->bitmap, (cache->bitmap_length)*sizeof(bitmap_entry_t));
	slab->type = empty;

	for (i = 0; i < cache->bitmap_length; i++)
	{
		slab->bitmap[i] = BITMAP_EMPTY;
	}

	
	if (ctor)
	{
		for (i = 0; i < cache->obj_per_slab; i++)
		{
			ctor(ptr_offset(slab->objects, i*(cache->object_size)));
		}
	}
	

	return slab;
}


// Destroy a slab (must be detached before)
void slab_free(slab_t *slab, unsigned char call_dtor)
{
	val_exp(slab != NULL);

	block_area_t hook = slab->my_hook;
	kmem_cache_t *cache = slab->cache;
	void(*dtor)(void*) = cache->dtor;
	int i;

	if (dtor && call_dtor)
	{
		for (i = 0; i < cache->obj_per_slab; i++)
		{
			dtor(ptr_offset(slab->objects, i*cache->object_size));
		}
	}


	mem_free(hook);
}

// Puts the slab in the adequate list of owner cache
void slab_attach(slab_t *slab)
{
	val_exp(slab != NULL);

	kmem_cache_t *cache = slab->cache;
	slab_type_t type = slab->type;

	slab->next = cache->heads[type];
	cache->heads[type] = slab;

	cache->slab_count[type]++;
}


// Removes the slab from owner cache list
int slab_detach(slab_t *slab)
{
	val_exp(slab != NULL);

	kmem_cache_t *cache = slab->cache;
	slab_type_t type = slab->type;
	slab_t *cur, *prev;

	prev = NULL;
	cur = cache->heads[type];

	while (cur != slab)
	{
		prev = cur;
		cur = cur->next;
		if (cur == NULL)
			return -1;
	}

	if (prev)
	{
		prev->next = cur->next;
	}
	else
	{
		cache->heads[type] = cur->next;
	}

	cache->slab_count[type]--;

	return 0;
}


// Changes slab type and updates lists
int slab_change_type(slab_t *slab, slab_type_t new_type)
{
	val_exp(slab != NULL);

	if (slab->type == new_type || slab_detach(slab) != 0)
		return -1;

	slab->type = new_type;
	slab_attach(slab);

	return 0;
}

// Allocate one object from slab (and change slab state)
void *slab_alloc_object(slab_t *slab)
{
	val_exp(slab != NULL);

	index_t obj_index, i ,j;
	bitmap_entry_t *bitmap = slab->bitmap; 
	kmem_cache_t *cache = slab->cache;
	void *obj;

	for (i = 0; i < cache->bitmap_length; i++)
	{
		if (bitmap[i] != BITMAP_FULL)
		{
			j = 0;
			while (bitmap[i] & (1 << j)) j++;

			obj_index = i * BITMAP_ENTRY_BITS + j;
			break;
		}
	}

	bitmap_set_used(bitmap, obj_index);

	slab->used_count++;

	if (slab->used_count < cache->obj_per_slab && slab->type != partial)
	{
		slab_change_type(slab, partial);
	}
	else if(slab->used_count == cache->obj_per_slab)
	{
		slab_change_type(slab, full);
	}

	obj = ptr_offset(slab->objects, obj_index*(slab->cache->object_size));


	return obj;
}


// Set one object free in slab
int slab_free_object(slab_t *slab, void *obj)
{
	val_exp(slab != NULL && obj != NULL);

	index_t obj_index;
	void *start_addr = slab->objects;
	void *end_addr = ptr_offset(start_addr, (slab->cache->obj_per_slab - 1)*(slab->cache->object_size));
	void(*ctor)(void*), (*dtor)(void*);

	if (!(start_addr <= obj && end_addr >= obj))
		return -1;

	ctor = slab->cache->ctor;
	dtor = slab->cache->dtor;

	obj_index = ((char*)obj - (char*)start_addr) / (slab->cache->object_size);

	bitmap_set_free(slab->bitmap, obj_index);

	slab->used_count--;

	if (slab->used_count == 0)
	{
		slab_change_type(slab, empty);
	}
	else if(slab->type != partial)
	{
		slab_change_type(slab, partial);
	}


	#ifdef FREE_DTOR
	if (dtor)
		dtor(obj);
	#endif

	
	#ifdef FREE_CTOR
	if (ctor)
		ctor(obj);
	#endif

	return 0;
}





/*
	Cache implementation
*/

// Initialize kmem_cache_t structure
void kmem_cache_init(kmem_cache_t *cache, const char *name, size_t obj_size, void(*ctor)(void *), void(*dtor)(void *))
{
	val_exp(cache != NULL &&  obj_size > 0);
	
	size_t bitmap_size, free, slab_size, waste;
	unsigned int obj_count, slab_order;

	obj_count = bitmap_size = 0;
	slab_order = calc_slab_order(obj_size);

	slab_size = size_of_blocks(slab_order);
	free = slab_size - sizeof(slab_t);

	while (bitmap_size + obj_count*obj_size <= free)
	{
		obj_count++;
		bitmap_size = calc_bitmap_size(obj_count);
	}

	obj_count--;
	bitmap_size = calc_bitmap_size(obj_count);
	waste = free - (bitmap_size + obj_count*obj_size);


	strcpy(cache->name, name);
	cache->object_size = obj_size;

	cache->ctor = ctor;
	cache->dtor = dtor;

	cache->extended = -1;
	cache->bitmap_length = bitmap_size / sizeof(bitmap_entry_t);
	cache->obj_per_slab = obj_count;
	cache->next_offset = 0;
	cache->max_alignments = waste / CACHE_L1_LINE_SIZE + 1;
	cache->object_size = obj_size;
	cache->slab_order = slab_order;
	cache->next = NULL;
	cache->heads[empty] = cache->heads[partial] = cache->heads[full] = NULL;
	cache->slab_count[empty] = cache->slab_count[partial] = cache->slab_count[full] = 0;
	cache->error = (error_code_t)0;

	cache->mutex = (mutex_t)cache->mutex_space;
	initMutex(cache->mutex);

}


// Add a new free slab to the cache
int kmem_cache_new_slab(kmem_cache_t *cache)
{
	val_exp(cache != NULL);

	slab_t *new_slab = slab_alloc(cache, cache->next_offset);

	cache->next_offset = (cache->next_offset + 1) % cache->max_alignments;

	if (new_slab == NULL)
	{
		cache->error = err_cache_expand;
		return -1;
	}

	slab_attach(new_slab);
	return 0;

}


// Add cache to global list
void kmem_cache_list_add(kmem_cache_t *cache)
{
	cache->next = kmem_ctrl->cache.next;
	kmem_ctrl->cache.next = cache;
}


// Remove cache from global list
int kmem_cache_list_remove(kmem_cache_t *cache)
{
	kmem_cache_t *cur = kmem_ctrl->cache.next, *prev = NULL;

	while (cur != cache)
	{
		prev = cur;
		cur = cur->next;
	}

	if (cur == NULL)
		return -1;

	if (prev)
	{
		prev->next = cur->next;
	}
	else
	{
		kmem_ctrl->cache.next = cur->next;
	}

	cur->next = NULL;
	return 0;
}


// Initialize allocator
void kmem_init(void *space, int block_num)
{
	val_exp(space != NULL && block_num > 0);

	unsigned int order;
	size_t size;
	char name[CACHE_NAME_LEN];

	buddy_init(space, block_num);
	kmem_ctrl = (kmem_ctrl_t*)kernel_ctrl_alloc(sizeof(kmem_ctrl_t));

	sem = (mutex_t)kernel_ctrl_alloc(MUTEX_SIZE);
	val_exp(sem != NULL);
	initMutex(sem);

	buddy_sem = (mutex_t)kernel_ctrl_alloc(MUTEX_SIZE);
	val_exp(buddy_sem != NULL);
	initMutex(buddy_sem);

	kmem_cache_init(&(kmem_ctrl->cache), "kmem_cache", sizeof(kmem_cache_t), NULL, NULL);

	kmem_cache_new_slab(&(kmem_ctrl->cache));

	for (order = MIN_BUFF_ORDER; order <= MAX_BUFF_ORDER; order++)
	{
		size = power_of_two(order);
		sprintf(name, "Buffer_%d", order);
		kmem_cache_init(&(kmem_ctrl->buffers[order - MIN_BUFF_ORDER].cache), name, size, NULL, NULL);
		kmem_ctrl->buffers[order - MIN_BUFF_ORDER].used = 0;
	}

}


// Allocate one object from cache
void *kmem_cache_alloc_obj(kmem_cache_t *cachep)
{
	void *obj = NULL;

	if (cachep->heads[partial])
	{
		obj = slab_alloc_object(cachep->heads[partial]);
	}
	else
	{
		if (cachep->heads[empty] == NULL)
		{
			if (kmem_cache_new_slab(cachep) != 0)
			{
				cachep->error = err_cache_obj_alloc;
				return NULL;
			}


			if (cachep->extended != -1)
				cachep->extended = 1;
		}

		obj = slab_alloc_object(cachep->heads[empty]);
	}

	return obj;
}


// Allocate one object from cache (thread-safe)
void *kmem_cache_alloc(kmem_cache_t *cachep)
{
	void *obj = NULL;

	arg_check_null(cachep != NULL);

	wait(sem);

	obj = kmem_cache_alloc_obj(cachep);
	
	signal(sem);

	return obj;
}

// Find cache with a specific name
kmem_cache_t *kmem_cache_find(char *name)
{
	kmem_cache_t *cur = kmem_ctrl->cache.next;

	while (cur)
	{
		if (strcmp(name, cur->name) == 0)
			return cur;
		cur = cur->next;
	}

	return NULL;
}


// Create cache
kmem_cache_t *kmem_cache_create(const char *name, size_t size, void(*ctor)(void *), void(*dtor)(void *))
{
	kmem_cache_t *cache = NULL;
	arg_check_null(name != NULL && size != 0);

	wait(sem);

	cache = kmem_cache_find(name);

	if (cache == NULL)
	{
		cache = (kmem_cache_t*)kmem_cache_alloc_obj(&(kmem_ctrl->cache));
		ret_check_null(cache, err_cache_create, sem);

		kmem_cache_init(cache, name, size, ctor, dtor);

		kmem_cache_list_add(cache);
	}
	
	signal(sem);
	
	return cache;
}


// Shrink cache
int kmem_cache_shrink(kmem_cache_t *cachep)
{
	unsigned int free_slabs = 0;
	slab_t *slab,*next;

	arg_check_null(cachep != NULL);

	wait(cachep->mutex);

	if (!(cachep->extended) && cachep->heads[empty] || cachep->extended == -1)
	{
		slab = cachep->heads[empty];
		while (slab)
		{
			next = slab->next;
			slab_detach(slab);
			slab_free(slab,0);
			slab = next;

			free_slabs++;
		}
	}

	cachep->extended = 0;

	signal(cachep->mutex);

	return (int)(free_slabs * power_of_two(cachep->slab_order));
}


// Set one object free from cache
int kmem_cache_free_obj(kmem_cache_t *cachep, void *objp)
{
	slab_t *slab;
	unsigned int i;

	for (i = partial; i <= full; i++)
	{
		slab = cachep->heads[i];
		while (slab)
		{
			if (slab_free_object(slab, objp) == 0)
			{
				return 0;
			}
			slab = slab->next;
		}
	}

	cachep->error = err_cache_obj_free;

	return -1;
}


// Set one object free from cache (thread-safe)
void kmem_cache_free(kmem_cache_t *cachep, void *objp)
{
	arg_check(cachep != NULL && objp != NULL);

	wait(cachep->mutex);

	kmem_cache_free_obj(cachep, objp);

	signal(cachep->mutex);
}


// Destroy cache
void kmem_cache_destroy(kmem_cache_t *cachep)
{
	unsigned int i;
	slab_t *slab, *next;

	arg_check(cachep != NULL);

	wait(cachep->mutex);

	for (i = empty; i <= full; i++)
	{
		slab = cachep->heads[i];
		while (slab)
		{
			next = slab->next;
			slab_detach(slab);
			slab_free(slab, 1);
			slab = next;
		}
	}

	val_exp(kmem_cache_list_remove(cachep)==0);

	kmem_cache_free_obj(&(kmem_ctrl->cache), cachep);

	signal(cachep->mutex);

}


// Allocate one small memmory buffer
void *kmalloc(size_t size)
{
	unsigned int order = 0;
	void *buff = NULL;

	arg_check_null(size != 0);

	wait(sem);

	while (power_of_two(order) < size) order++;
	
	kmem_ctrl->buffers[order - MIN_BUFF_ORDER].used = 1;

	
	buff = kmem_cache_alloc_obj(&(kmem_ctrl->buffers[order - MIN_BUFF_ORDER].cache));
	ret_check_null(buff, err_buff_alloc,sem);
	
	signal(sem);

	return buff;

}


// Set one small memmory buffer free
void kfree(const void *objp)
{
	index_t index;
	slab_t *slab;
	unsigned int i;
	kmem_cache_t *cachep;

	arg_check(objp != NULL);

	wait(sem);

	for (index = 0; index < SIZE_N_COUNT; index++)
	{
		if (kmem_ctrl->buffers[index].used)
		{
			cachep = &(kmem_ctrl->buffers[index].cache);

			for (i = partial; i <= full; i++)
			{
				slab = cachep->heads[i];
				while (slab)
				{
					if (slab_free_object(slab, (void*)objp) == 0)
					{
						signal(sem);
						return;
					}
					slab = slab->next;
				}
			}
		}

	}

	print_error(err_buff_free);

	signal(sem);

}


// Print cache info
void kmem_cache_info(kmem_cache_t *cachep)
{
	unsigned int total_obj, used_obj, total_slabs;
	double usage = 0;
	slab_t *slab;

	arg_check(cachep != NULL);

	wait(sem);

	total_slabs = cachep->slab_count[empty] + cachep->slab_count[partial] + cachep->slab_count[full];

	total_obj = total_slabs * cachep->obj_per_slab;

	used_obj = 0;

	slab = cachep->heads[partial];
	while (slab)
	{
		used_obj += slab->used_count;
		slab = slab->next;
	}

	used_obj += cachep->slab_count[full] * cachep->obj_per_slab;

	if(total_obj)
		usage = 100 * ((double)used_obj / total_obj);

	printf_s("\nCache info\n");
	printf_s("Name: %s\n",cachep->name);
	printf_s("Object size: %d\n",cachep->object_size);
	printf_s("Cache size in blocks: %d\n", size_in_blocks(sizeof(kmem_cache_t)) + total_slabs*(power_of_two(cachep->slab_order)));
	printf_s("Number of slabs: %d\n", total_slabs);
	printf_s("Objects per slab: %d\n", cachep->obj_per_slab);
	printf_s("Used space: %.1f%%\n\n", usage);

	signal(sem);

}


// Print cache error
int kmem_cache_error(kmem_cache_t *cachep)
{
	arg_check_null(cachep != NULL);

	wait(sem);

	error_code_t error = cachep->error;

	if (error)
	{
		print_error(error);
	}

	signal(sem);

	return error;
}


