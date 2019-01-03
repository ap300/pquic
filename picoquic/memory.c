#include "memory.h"
#include "memcpy.h"

/* This code is highly inspired from https://github.com/fouady/simple-malloc/blob/master/mem_management.cpp */

#include <unistd.h>
#include "picoquic_internal.h"

/**
 * MEM_BUFFER determines size of RAM
 * METADATA_SIZE is the fixed size of metadata block
 * ALIGNMENT_FACTOR determines the smallest chunk of memory in bytes.
 * MAGIC_NUMBER is used to check if the pointer to be freed is valid.
 */
#define MEM_BUFFER PLUGIN_MEMORY
#define METADATA_SIZE (sizeof(meta_data))
#define ALIGNMENT_FACTOR 4
#define MAGIC_NUMBER 0123

/**
 * This structure contains the metadata.
 * Size determines the size of data excuding the size of metadata
 * next block is the pointer to next slot of memory in heap.
 */
typedef struct meta_data {
	unsigned int size:31;
	unsigned int available:1;
	unsigned int magic_number;
} meta_data;

/**
 * Adjusts the requested size so that the allocated space is always a multiple of alighment factor
 */ 
unsigned int align_size(unsigned int size) {
	return (size % ALIGNMENT_FACTOR) ? size + ALIGNMENT_FACTOR - (size % ALIGNMENT_FACTOR) : size;
}

/**
 * Home-made implementation of sbrk within a given protoop_plugin_t.
 */
void *my_sbrk(protoop_plugin_t *p, intptr_t increment) {
    if (p->heap_end + increment - p->heap_start > MEM_BUFFER) {
        /* Out of memory */
        return NULL;
    }

    p->heap_end += increment;
    return p->heap_end;
}

meta_data *get_next_slot(protoop_plugin_t *p, meta_data *iter) {
	if ((char *)iter < (char *) p->heap_last_block) {
		return (meta_data *) ((char *) iter + METADATA_SIZE + iter->size);
	}
	return NULL; 
}

/**
 * Goes through the whole heap to find an empty slot.
 */ 
meta_data *find_slot(protoop_plugin_t *p, unsigned int size) {
	meta_data *iter = (meta_data*) p->heap_start;
	while(iter && iter->magic_number == MAGIC_NUMBER) {
		if (iter->available && iter->size >= size) {
			iter->available = 0;
			return iter;
		}
		iter = get_next_slot(p, iter);
	}
	return NULL;
}

/**
 * If a free slot can accommodate atleast 1 more (METADATA_SIZE + ALIGNMENT FACTOR)
 * apart from the requested size, then the slot is divided to save space.
 */ 
void divide_slot(protoop_plugin_t *p, void *slot, unsigned int size) {
	meta_data *slot_to_divide = (meta_data *) slot;
	meta_data *new_slot= (meta_data*) (((char *) slot_to_divide) + METADATA_SIZE + size);
	
	new_slot->size=slot_to_divide->size - size - METADATA_SIZE;
	new_slot->available = 1;
	new_slot->magic_number = MAGIC_NUMBER;

	slot_to_divide->size = size;
	if (p && (char *) slot_to_divide == p->heap_last_block) {
	    p->heap_last_block = (char *) new_slot;
	}
}

/**
 * Extends the heap using sbrk syscall. 
 */
void *extend(protoop_plugin_t *p, unsigned int size) {
	meta_data *new_block = (meta_data*) my_sbrk(p, 0);
	if ((char*) new_block - (char*) p->heap_start > MEM_BUFFER) return NULL;
	int *flag = (int *) my_sbrk(p, size + METADATA_SIZE);
	if (!flag) {
		printf("Out of memory!\n");
		return NULL;
	}
	new_block->size = size;
	new_block->available = 0;
	new_block->magic_number = MAGIC_NUMBER;
	
	p->heap_last_block = (char *) new_block;
	return new_block;
}

/**
 * Returns the metadata from heap corresponding to a data pointer.
 */ 
meta_data* get_metadata(void *ptr) {
	return (meta_data *)((char *) ptr - METADATA_SIZE);
}

/**
* Search for big enough free space on heap.
* Split the free space slot if it is too big, else space will be wasted.
* Return the pointer to this slot.
* If no adequately large free slot is available, extend the heap and return the pointer.
*/
void *my_malloc(picoquic_cnx_t *cnx, unsigned int size) {
	protoop_plugin_t *p = cnx->current_plugin;
	if (!p) {
		fprintf(stderr, "FATAL ERROR: calling my_malloc outside plugin scope!\n");
		exit(1);
	}
	size = align_size(size);
	void *slot;
	if (p->heap_start){
        DBG_MEMORY_PRINTF("Heap starts at: %p", p->heap_start);
		slot = find_slot(p, size);
		if (slot) {
			if (((meta_data *) slot)->size > size + METADATA_SIZE) {
				divide_slot(p, slot, size);
			}
		} else {
			slot = extend(p, size);
		}
	} else {
		p->heap_start = my_sbrk(p, 0);
        DBG_MEMORY_PRINTF("Heap starts at: %p", p->heap_start);
		slot = extend(p, size);
	}

	if (!slot) { return slot; }

    DBG_MEMORY_PRINTF("Memory assigned from %p to %p", slot, (void *)((char *) slot + METADATA_SIZE + ((meta_data *) slot)->size));
    DBG_MEMORY_PRINTF("Memory ends at: %p", my_sbrk(p, 0));
    DBG_MEMORY_PRINTF("Size of heap so far: 0x%lx", (unsigned long) ((char *) my_sbrk(p, 0) - (char *) p->heap_start));

	return ((char *) slot) + METADATA_SIZE;
}

/**
 * Frees the allocated memory. If first checks if the pointer falls
 * between the allocated heap range. It also checks if the pointer
 * to be deleted is actually allocated. this is done by using the
 * magic number. Due to lack of time i haven't worked on fragmentation.
 */ 
void my_free(picoquic_cnx_t *cnx, void *ptr) {
	protoop_plugin_t *p = cnx->current_plugin;
	if (!p) {
		fprintf(stderr, "FATAL ERROR: calling my_free outside plugin scope!\n");
		exit(1);
	}
	if (!p->heap_start) return;
	if ((char *) ptr >= p->heap_start + METADATA_SIZE && ptr < my_sbrk(p, 0)) {
		meta_data *ptr_metadata = get_metadata(ptr);
		if (ptr_metadata->magic_number == MAGIC_NUMBER) {
			ptr_metadata->available = 1;
            DBG_MEMORY_PRINTF("Memory freed at: %p", ptr_metadata);
		}
	}
}

void my_free_in_core(protoop_plugin_t *p, void *ptr) {
	if (!p->heap_start) return;
	if ((char *) ptr >= p->heap_start + METADATA_SIZE && ptr < my_sbrk(p, 0)) {
		meta_data *ptr_metadata = get_metadata(ptr);
		if (ptr_metadata->magic_number == MAGIC_NUMBER) {
			ptr_metadata->available = 1;
            DBG_MEMORY_PRINTF("Memory freed at: %p", ptr_metadata);
		}
	}
}

/**
 * Reallocate the allocated memory to change its size. Three cases are possible.
 * 1) Asking for lower or equal size, or larger size without any block after.
 *    The block is left untouched, we simply increase its size.
 * 2) Asking for larger size, and another block is behind.
 *    We need to request another larger block, then copy the data and finally free it.
 * 3) Asking for larger size, without being able to have free space.
 *    Free the pointer and return NULL.
 * If an invalid pointer is provided, it returns NULL without changing anything.
 */
void *my_realloc(picoquic_cnx_t *cnx, void *ptr, unsigned int size) {
    /* If no previous ptr, fast-track to my_malloc */
    if (!ptr) return my_malloc(cnx, size);
	protoop_plugin_t *p = cnx->current_plugin;
	if (!p) {
		p = (protoop_plugin_t *) cnx;
		// fprintf(stderr, "FATAL ERROR: calling my_realloc outside plugin scope!\n");
		// exit(1);
	}
    /* If the previous ptr is invalid, return NULL */
    if ((char *) ptr < p->heap_start + METADATA_SIZE && ptr >= my_sbrk(p, 0)) return NULL;
    /* Now take metadata */
    meta_data *ptr_metadata = get_metadata(ptr);
    if (ptr_metadata->magic_number != MAGIC_NUMBER) {
        /* Invalid pointer */
        return NULL;
    }
    /* Case 1a and 1b */
    unsigned int old_size = ptr_metadata->size;
    /* This case is broken...
    if (size <= old_size) {
        ptr_metadata->size = size;
        return ptr;
    }
    */

    /* This is clearly not the most optimized way, but it will always work */
    void *new_ptr = my_malloc(cnx, size);
    if (!new_ptr) {
        my_free(cnx, ptr);
        return NULL;
    }
    my_memcpy(new_ptr, ptr, old_size > size ? size : old_size);
    my_free(cnx, ptr);
    return new_ptr;
}

void init_memory_management(protoop_plugin_t *p)
{
	p->heap_start = p->memory;
	p->heap_end = p->memory;
	p->heap_last_block = NULL;
}