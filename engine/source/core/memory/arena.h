#pragma once

/*
* Inspired by: https://andreleite.com/posts/2025/nstl/virtual-memory-arena-allocator/
*/

// TODO: in ibx_memory.h, include statistic concerning arena allocation.

#include "defines.h"

#include "core/logger.h"
#include "ibx_memory.h"

struct arena_t;

#define ARENA_PTR arena_t*

struct arena_t{
    u8* base_ptr;
    u64 reserved_size; // This is the reserved size of the arena (e.g., 1GB)
    u64 commited_size; // This is the size of the arena that is currently commited (e.g., 256MB)    
    u64 current_offset; // This is the offset of the next allocation
};

// Create the area.
// This function reserves virtual memory for the arena and initializes its metadata.
ibx__api__ ARENA_PTR arena_initialize(u64 reserve_size);
ibx__api__ VOID_PTR arena_allocate(ARENA_PTR arena, u64 size);

ibx__api__ void arena_reset(ARENA_PTR arena);
ibx__api__ void arena_terminate(ARENA_PTR arena);