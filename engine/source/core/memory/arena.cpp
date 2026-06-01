#include "arena.h"

#include "platform/platform_commons.h"

// Align the required commit size up to the nearest page
#define _aligment_to_nearest_page_size(_v) ((_v + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE)

ARENA_PTR arena_initialize(u64 reserve_size)
{
    ARENA_PTR out_arena = (ARENA_PTR)ibx_memory_allocator(sizeof(arena_t), MEMORY_TAG_ARENA);
    if (!out_arena)
    {
        IBX_LOG_ERROR("Failed to allocate memory for arena !");
        return 0;
    } 

    // Align reservation up to the nearest page size
    reserve_size = _aligment_to_nearest_page_size(reserve_size); 

    VOID_PTR block = ibx_memory_allocator_virtual_memory_reserve(reserve_size);
    if (!block)
    {
        IBX_LOG_ERROR("Failed to reserve virtual memory for arena !");
        ibx_memory_free(out_arena, sizeof(arena_t), MEMORY_TAG_ARENA);
        return 0;
    }


    out_arena->base_ptr = (u8*)block;
    out_arena->reserved_size = reserve_size;
    out_arena->commited_size = 0;
    out_arena->current_offset = 0;

    return out_arena;
}

VOID_PTR arena_allocate(ARENA_PTR arena, u64 size)
{
    if(!arena || size == 0)
    {
        IBX_LOG_ERROR("Invalid arena or size for allocation !");
        return 0;
    }

    u64 new_offset = arena->current_offset + size;

    if (new_offset > arena->reserved_size)
    {
        IBX_LOG_ERROR("Arena out of reserved space !");
        return 0;
    }

    if (new_offset > arena->commited_size)
    {
        u64 new_commit_target = _aligment_to_nearest_page_size(new_offset);
        // Clamp to the reservation limit
        if (new_commit_target > arena->reserved_size) {
            new_commit_target = arena->reserved_size;
        }

        u64 size_to_commit = new_commit_target - arena->commited_size;
        VOID_PTR commit_start_adress = arena->base_ptr + arena->commited_size;

        if (!ibx_memory_allocator_virtual_memory_commit(commit_start_adress, size_to_commit))
        {
            IBX_LOG_ERROR("Unable to commit memory !");
            return 0;
        }

        arena->commited_size = new_commit_target;
    }

    VOID_PTR memory = arena->base_ptr + arena->current_offset;
    arena->current_offset = new_offset;
    
    return(memory);
}

// Reset the offset to zero.
void arena_reset(ARENA_PTR arena){
    arena->current_offset = 0;
}

// Release memory to the OS completely.
void arena_terminate(ARENA_PTR arena){
    ibx_memory_virtual_free(arena->base_ptr, 0); // base_ptr is a pointer to the beginning of the entire block of memory. With size set to 0, the entire block is released. 
    ibx_memory_free(arena, sizeof(arena_t), MEMORY_TAG_ARENA);
}


