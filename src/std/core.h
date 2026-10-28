#pragma once

// #ifdef _MSC_VER
// #   define _CRT_SECURE_NO_WARNINGS
// #endif

#include <math.h>
#include <stddef.h>  // ptrdiff_t, for the `ptrdiff` alias below
#include <stdint.h>
#include <inttypes.h>  // PRIu64 and friends: u64 is unsigned long on Linux, unsigned long long on Windows
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define null NULL

typedef float f32;
typedef double f64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef size_t usize;

/* `bool` is one byte, so a boolean family whose names carry a width could not be
   built on it -- b8 and b32 would have been the same type twice. Each is the
   matching fixed-width integer instead. b32 is the default boolean: it is what
   comparisons and `&&`/`||` produce, and it matches Win32's BOOL. Note these do
   not normalise to 0/1 the way C's bool does. */
typedef uint8_t  b8;
typedef uint16_t b16;
typedef int32_t  b32;
typedef int64_t  b64;

/* rin's name for C's char: whatever width and signedness the C compiler on
   this target gives it, which is exactly the point -- the language declines to
   have an opinion rather than pretending the question is settled. */
typedef char c8;

/* Spellings of C's own fixed names, so a program can say what it means without
   reaching for a cinclude. */
typedef intptr_t  intptr;
typedef uintptr_t uintptr;
typedef ptrdiff_t ptrdiff;
typedef intmax_t  intmax;
typedef uintmax_t uintmax;

typedef void *voidptr;
// typedef i8 *str;
// typedef const str *cstr;
typedef const char *str;
typedef str *strptr;

#define static_internal static
#define static_global static
#define static_local static

#define Kilobytes(N) ((u64)(N) * 1024)
#define Megabytes(N) ((u64)Kilobytes(N) * 1024)
#define Gigabytes(N) ((u64)Megabytes(N) * 1024)

#define stringify(s) #s
#define concat(a, b) a##b
// #define pi 3.141592f
#define sizeofarray(array) (sizeof(array) / sizeof(array[0]))
#define cast(T, V) (T)(V)

#define primdecl(primname)
#define enumdecl(enumname) typedef enum enumname enumname
#define enumdef(enumname) typedef enum enumname enumname; enum enumname
#define uniondecl(unionname) typedef union unionname unionname
#define uniondef(unionname) typedef union unionname unionname; union unionname
#define structdecl(structname) typedef struct structname structname
#define structdef(structname) typedef struct structname structname; struct structname

#define template(...)

/* For static inline helpers a file may never call. clang warns about an unused
   static inline function in the main file, and --single-file puts core.h
   there, so a -Werror build of a program with no slices would fail. */
#if defined(__GNUC__) || defined(__clang__)
#define RIN_MAYBE_UNUSED __attribute__((unused))
#else
#define RIN_MAYBE_UNUSED
#endif

/* print<u64> and print<i64> in std/print.rin, which cannot name a format
   macro from rin. */
RIN_MAYBE_UNUSED static inline void rin_print_u64(u64 x) { printf("%" PRIu64, x); }
RIN_MAYBE_UNUSED static inline void rin_print_i64(i64 x) { printf("%" PRId64, x); }

/* rin's bounds checks. Indexing an array with a value only known at run time,
   or indexing a slice, goes through RIN_INDEX; a subrange `x[a..b]` goes
   through RIN_RANGE. Both are checked unless NDEBUG is defined -- C's own
   switch, which a Release build already sets -- and then they are the plain
   index. A failed check names the rin line, because the generated C carries
   #line directives, and aborts. A negative index is checked as unsigned, so it
   fails the same comparison as one past the end. */
#ifdef NDEBUG
#define RIN_INDEX(i, n) (i)
#define RIN_RANGE(a, b, n) (a)
#else
RIN_MAYBE_UNUSED static inline u64 rin_index_check(u64 i, u64 n, const char *file, int line) {
    if (i >= n) {
        if ((i64)i < 0) {
            fprintf(stderr, "%s:%d: index %lld out of bounds (length %llu)\n", file, line,
                    (long long)(i64)i, (unsigned long long)n);
        } else {
            fprintf(stderr, "%s:%d: index %llu out of bounds (length %llu)\n", file, line,
                    (unsigned long long)i, (unsigned long long)n);
        }
        abort();
    }
    return i;
}
RIN_MAYBE_UNUSED static inline u64 rin_range_check(u64 a, u64 b, u64 n, const char *file, int line) {
    if (a > b || b > n) {
        fprintf(stderr, "%s:%d: range %llu..%llu out of bounds (length %llu)\n", file, line,
                (unsigned long long)a, (unsigned long long)b, (unsigned long long)n);
        abort();
    }
    return a;
}
#define RIN_INDEX(i, n) rin_index_check((u64)(i), (u64)(n), __FILE__, __LINE__)
#define RIN_RANGE(a, b, n) rin_range_check((u64)(a), (u64)(b), (u64)(n), __FILE__, __LINE__)
#endif

/* `noalias` parameters (docs/noalias-variadic.md). RIN_RESTRICT is C's
   restrict, spelled the way every compiler accepts in C whatever the standard
   flag; RIN_ALWAYS_INLINE marks the helper a proc with noalias slices is moved
   into. clang-cl defines _MSC_VER too, and takes the attribute. MSVC's cl
   ignores __forceinline in Debug (/Od), so there the helper stays a call. */
#define RIN_RESTRICT __restrict
#if defined(_MSC_VER) && !defined(__clang__)
#define RIN_ALWAYS_INLINE static __forceinline
#else
#define RIN_ALWAYS_INLINE static inline __attribute__((always_inline))
#endif

/* The Debug check that the promise holds, at the top of the proc. Each of the
   n entries is (pointer, extent in bytes, name, flags): RIN_NOALIAS_PARAM if
   the parameter is noalias, RIN_NOALIAS_WRITE if it can write. A pair is
   checked when one side is noalias and one side can write, so the check
   assumes any non-const parameter may be written. An extent of RIN_NOALIAS_ANY
   is unknown, a `^T`, and only an equal address counts; an extent of 0, an
   empty slice, overlaps nothing. */
#define RIN_NOALIAS_PARAM 1
#define RIN_NOALIAS_WRITE 2
#define RIN_NOALIAS_ANY (~(u64)0)
#ifdef NDEBUG
#define RIN_NOALIAS(...) ((void)0)
#else
#include <stdarg.h>
RIN_MAYBE_UNUSED static inline void rin_noalias_check(const char *file, int line, int n, ...) {
    const u8 *ptr[16];
    u64 bytes[16];
    const char *name[16];
    int flags[16];
    va_list args;
    va_start(args, n);
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) {
        ptr[i] = (const u8 *)va_arg(args, const void *);
        bytes[i] = va_arg(args, u64);
        name[i] = va_arg(args, const char *);
        flags[i] = va_arg(args, int);
    }
    va_end(args);
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (!((flags[i] | flags[j]) & RIN_NOALIAS_PARAM)) continue;
            if (!((flags[i] | flags[j]) & RIN_NOALIAS_WRITE)) continue;
            if (!ptr[i] || !ptr[j] || bytes[i] == 0 || bytes[j] == 0) continue;
            bool overlap;
            if (bytes[i] == RIN_NOALIAS_ANY || bytes[j] == RIN_NOALIAS_ANY) {
                overlap = ptr[i] == ptr[j];
            } else {
                overlap = ptr[i] < ptr[j] + bytes[j] && ptr[j] < ptr[i] + bytes[i];
            }
            if (overlap) {
                fprintf(stderr, "%s:%d: noalias parameters '%s' and '%s' overlap\n", file, line, name[i], name[j]);
                abort();
            }
        }
    }
}
#define RIN_NOALIAS(n, ...) rin_noalias_check(__FILE__, __LINE__, n, __VA_ARGS__)
#endif

#ifdef __cplusplus
    #define haikal_alignof(type) alignof(type)
#else
    #define haikal_alignof(type) _Alignof(type)
#endif
