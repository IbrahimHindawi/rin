#pragma once
#include <stdint.h>
#include <stdlib.h>

#ifdef _WIN32
/* The three kernel32 functions memops needs, declared here instead of
   including <windows.h>. Every rin program includes std, and <windows.h>
   defines macros such as `near`, `far`, `min` and `max`, which would turn a
   rin local of that name into broken C. The declarations match the SDK's
   exactly, so a program that includes <windows.h> itself still compiles; the
   numbers are the SDK's MEM_ and PAGE_ constants. */
#if defined(_WIN64)
typedef unsigned long long memops_os_size;
#else
typedef unsigned long memops_os_size;
#endif
struct _SYSTEM_INFO;
__declspec(dllimport) void *__stdcall VirtualAlloc(void *address, memops_os_size size,
                                                   unsigned long type, unsigned long protect);
__declspec(dllimport) void __stdcall GetSystemInfo(struct _SYSTEM_INFO *info);
__declspec(dllimport) void __stdcall DebugBreak(void);

/* SYSTEM_INFO's layout, under a name of its own so it cannot clash with the
   SDK's when both are seen. */
typedef struct memops_os_system_info {
    unsigned long oem_id;
    unsigned long page_size;
    void *min_address;
    void *max_address;
    uintptr_t processor_mask;
    unsigned long processor_count;
    unsigned long processor_type;
    unsigned long allocation_granularity;
    unsigned short processor_level;
    unsigned short processor_revision;
} memops_os_system_info;
#if defined(_WIN64)
/* SYSTEM_INFO is 48 bytes on 64-bit Windows. An array check rather than
   _Static_assert, which cl rejects outside /std:c11. */
typedef char memops_os_system_info_is_48_bytes[sizeof(memops_os_system_info) == 48 ? 1 : -1];
#endif

static inline uint64_t memops_os_page_size(void) {
    memops_os_system_info info = {0};
    GetSystemInfo((struct _SYSTEM_INFO *)&info);
    return (uint64_t)info.page_size;
}

static inline uint8_t *memops_os_reserve(uint64_t size) {
    return (uint8_t *)VirtualAlloc(NULL, (memops_os_size)size, 0x00002000 /* MEM_RESERVE */, 0x01 /* PAGE_NOACCESS */);
}

static inline void *memops_os_commit(void *ptr, uint64_t size) {
    return VirtualAlloc(ptr, (memops_os_size)size, 0x00001000 /* MEM_COMMIT */, 0x04 /* PAGE_READWRITE */);
}

static inline void memops_os_debug_break(void) {
    DebugBreak();
}
#else
#include <sys/mman.h>
#include <unistd.h>

/* The same reserve-then-commit as VirtualAlloc: address space now, memory as
   the arena grows. MAP_NORESERVE keeps a large reservation from counting
   against overcommit. macOS spells MAP_ANONYMOUS as MAP_ANON and has no
   MAP_NORESERVE, which it does not need. */
#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

static inline uint64_t memops_os_page_size(void) {
    long size = sysconf(_SC_PAGESIZE);
    return size > 0 ? (uint64_t)size : 4096;
}

static inline uint8_t *memops_os_reserve(uint64_t size) {
    void *p = mmap(NULL, (size_t)size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : (uint8_t *)p;
}

static inline void *memops_os_commit(void *ptr, uint64_t size) {
    return mprotect(ptr, (size_t)size, PROT_READ | PROT_WRITE) == 0 ? ptr : NULL;
}

static inline void memops_os_debug_break(void) {
}
#endif
