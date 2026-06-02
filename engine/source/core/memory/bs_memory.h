#pragma once

#include "defines.h"

// Based on the tag we will be able to aligned or not aligned memory.
typedef enum EMemoryTag{
    MEMORY_TAG_UNKNOWN,
    MEMORY_TAG_ARRAY,
    MEMORY_TAG_VECTOR,
    MEMORY_TAG_DICT,
    MEMORY_TAG_RING_QUEUE,
    MEMORY_TAG_BST,
    MEMORY_TAG_STRING,
    MEMORY_TAG_APPLICATION,
    MEMORY_TAG_JOB,
    MEMORY_TAG_TEXTURE,
    MEMORY_TAG_MATERIAL_INSTANCE,
    MEMORY_TAG_RENDERER,
    MEMORY_TAG_GAME,
    MEMORY_TAG_TRANSFORM,
    MEMORY_TAG_ENTITY,
    MEMORY_TAG_ENTITY_NODE,
    MEMORY_TAG_SCENE,
    MEMORY_TAG_ARENA,

    MEMORY_TAG_MAX_TAGS // Should always be the laste element.
} EMemoryTag;

// TODO: these shall not be exported !
bs__api__ void bs_memory_initialize();
bs__api__ void bs_memory_terminate();

bs__api__ VOID_PTR bs_memory_allocator_virtual_memory_commit(VOID_PTR block, u64 commit_size);
bs__api__ VOID_PTR bs_memory_allocator_virtual_memory_reserve(u64 resherve_size);
bs__api__ void bs_memory_virtual_free(VOID_PTR block, u64 size);

bs__api__ VOID_PTR bs_memory_allocator(u64 size, EMemoryTag tag);
bs__api__ void bs_memory_free(VOID_PTR block, u64 size, EMemoryTag tag);

bs__api__ VOID_PTR bs_memory_zero(VOID_PTR block, u64 size);
bs__api__ VOID_PTR bs_memory_copy(VOID_PTR dest, const VOID_PTR source, u64 size);
bs__api__ VOID_PTR bs_memory_set(VOID_PTR dest, i32 value, u64 size);

// Debug function. Prints usage statistics to the console.
bs__api__ char* bs_memory_get_memory_usage_string();

