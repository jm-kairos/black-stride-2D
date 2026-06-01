#include "ibx_memory.h"

#include "core/logger.h"
#include "platform/platform.h"

// TODO: Costum string lib
#include <string.h>
#include <stdio.h>

struct ibx_memory_stats
{
    u64 total_allocated;
    u64 tagged_allocations[MEMORY_TAG_MAX_TAGS];
};

static const char* ibx_memory_tag_strings[MEMORY_TAG_MAX_TAGS] = {
    "UNKNOWN    ",
    "ARRAY      ",
    "VECTOR     ",
    "DICT       ",
    "RING_QUEUE ",
    "BST        ",
    "STRING     ",
    "APPLICATION",
    "JOB        ",
    "TEXTURE    ",
    "MAT_INST   ",
    "RENDERER   ",
    "GAME       ",
    "TRANSFORM  ",
    "ENTITY     ",
    "ENTITY_NODE",
    "SCENE      ",
    "ARENA      "
};

static struct ibx_memory_stats stats; // Private to this CPP file.

void ibx_memory_initialize()
{
    platform_zero_memory(&stats, sizeof(stats));
}

void ibx_memory_terminate(){
    // CleanUp allocations.
}

VOID_PTR ibx_memory_allocator_virtual_memory_commit(VOID_PTR block, u64 commit_size)
{
    return platform_allocate_virtual_memory_commit(block, commit_size);
}

VOID_PTR ibx_memory_allocator_virtual_memory_reserve(u64 reserve_size)
{
    return platform_allocate_virtual_memory_reserve(reserve_size);
}

void ibx_memory_virtual_free(VOID_PTR block, u64 size){
    platform_virtual_free(block, size);
}

VOID_PTR ibx_memory_allocator(u64 size, EMemoryTag tag)
{
    if (tag == MEMORY_TAG_UNKNOWN)
        IBX_LOG_WARN("ibxmem_alloc called using MEMORY_TAG_UNKOWN. Describe this allocation.")

    stats.total_allocated += size;
    stats.tagged_allocations[tag] += size;

    // TODO: revisit memory alligment
    VOID_PTR block = platform_allocate(size, FALSE); // Pass false for now.
    platform_zero_memory(block, size);

    return(block);
}

void ibx_memory_free(VOID_PTR block, u64 size, EMemoryTag tag){   
    if (tag == MEMORY_TAG_UNKNOWN)
        IBX_LOG_WARN("ibxmem_free called using MEMORY_TAG_UNKOWN. Describe this allocation.")

    stats.total_allocated -= size;
    stats.tagged_allocations[tag] -= size;

    platform_free(block, FALSE);
}

VOID_PTR ibx_memory_zero(VOID_PTR block, u64 size){
    return platform_zero_memory(block, size);
}

VOID_PTR ibx_memory_copy(VOID_PTR dest, const VOID_PTR source, u64 size){
    return platform_copy_memory(dest, source, size);
}

VOID_PTR ibx_memory_set(VOID_PTR dest, i32 value, u64 size){
    return platform_set_memory(dest, value, size);
}

char *ibx_memory_get_memory_usage_string(){
    const u64 gib = 1024 * 1024 * 1024; 
    const u64 mib = 1024 * 1024;
    const u64 kib = 1024;

    char buffer[8000] = "System memory use (tagged): \n";
    u64 offset = strlen(buffer);
    for (u32 i = 0; i < MEMORY_TAG_MAX_TAGS; ++i)
    {
        char unit[4] = "xib";
        f32 amount = 1.0f;
        if (stats.tagged_allocations[i] >= gib)
        {
            unit[0] = 'g';
            amount = stats.tagged_allocations[i] / (f32)gib;
        } else if (stats.tagged_allocations[i] >= mib) {
            unit[0] = 'm';
            amount = stats.tagged_allocations[i] / (f32)mib;
        } else if (stats.tagged_allocations[i] >= kib) {
            unit[0] = 'k';
            amount = stats.tagged_allocations[i] / (f32)kib;
        }else{
            unit[0] = 'b';
            unit[1] = 0;
            amount = (f32)stats.tagged_allocations[i];
        }

        offset += snprintf(buffer + offset, 8000, " %s: %.2f%s\n", ibx_memory_tag_strings[i], amount, unit);;
    }
    char* out_string = _strdup(buffer); // Perform a string copy by allocating under the hood
    return out_string; // Need to release after use !
}
