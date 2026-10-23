#pragma once
#include <stdint.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static inline uint64_t memops_os_page_size(void) {
    SYSTEM_INFO systeminfo = {0};
    GetSystemInfo(&systeminfo);
    return (uint64_t)systeminfo.dwPageSize;
}

static inline uint8_t *memops_os_reserve(uint64_t size) {
    return (uint8_t *)VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE, PAGE_NOACCESS);
}

static inline void *memops_os_commit(void *ptr, uint64_t size) {
    return VirtualAlloc(ptr, (SIZE_T)size, MEM_COMMIT, PAGE_READWRITE);
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
