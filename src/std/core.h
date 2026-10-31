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
   through RIN_RANGE. Both are checked in every build, Release included: the
   safety rules rest on them, so they do not hang on NDEBUG. Only defining
   RIN_NO_BOUNDS_CHECKS, on purpose, makes them the plain index, and such a
   program has given up the guarantee. A failed check names the rin line, because the generated C carries
   #line directives, and aborts. A negative index is checked as unsigned, so it
   fails the same comparison as one past the end. */
#ifdef RIN_NO_BOUNDS_CHECKS
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
#define rin_index_check_(i, n, file, line) rin_index_check((i), (n), (file), (line))
#define rin_range_check_(a, b, n, file, line) rin_range_check((a), (b), (n), (file), (line))
#endif

/* A slice is read whole, so its data and its length are from the same moment. C
   leaves open the order of `b->xs.data`, `b->xs.length` and a call beside them, and
   a call may replace the slice: a new pointer checked against the old length would
   pass and write out of bounds. A slice is laid out { data, length }. */
typedef struct rin_slice_raw { u8 *data; u64 length; } rin_slice_raw;
RIN_MAYBE_UNUSED static inline void *rin_slice_at(const void *slice, u64 i, u64 size, const char *file, int line) {
    rin_slice_raw s;
    memcpy(&s, slice, sizeof(s));
#ifndef RIN_NO_BOUNDS_CHECKS
    rin_index_check_(i, s.length, file, line);
#endif
    return s.data + i * size;
}
RIN_MAYBE_UNUSED static inline void *rin_slice_range(const void *slice, u64 a, u64 b, u64 size, void *out, const char *file, int line) {
    rin_slice_raw s;
    memcpy(&s, slice, sizeof(s));
#ifndef RIN_NO_BOUNDS_CHECKS
    rin_range_check_(a, b, s.length, file, line);
#endif
    s.data += a * size;
    s.length = b - a;
    memcpy(out, &s, sizeof(s));
    return out;
}
RIN_MAYBE_UNUSED static inline void *rin_array_range(const void *data, u64 a, u64 b, u64 n, u64 size, void *out, const char *file, int line) {
    rin_slice_raw s;
#ifndef RIN_NO_BOUNDS_CHECKS
    rin_range_check_(a, b, n, file, line);
#endif
    s.data = (u8 *)data + a * size;
    s.length = b - a;
    memcpy(out, &s, sizeof(s));
    return out;
}

/* rin's integers are defined, in every build and with any C flags: signed + - *
   wrap, a division by zero stops the program, MIN / -1 wraps, and a float cast to an
   integer saturates with NaN as 0, as in Rust. The emitter goes through the unsigned
   type or through these, because C's overflow is undefined and an optimiser may use
   that to remove a bounds check. */
#define RIN_IDIV_DEFINE(T, U) \
    RIN_MAYBE_UNUSED static inline T rin_idiv_##T(T a, T b, const char *file, int line) { \
        if (b == 0) { fprintf(stderr, "%s:%d: division by zero\n", file, line); abort(); } \
        if ((T)-1 < 0 && b == (T)-1) return (T)((U)0 - (U)a); \
        return (T)(a / b); \
    } \
    RIN_MAYBE_UNUSED static inline T rin_irem_##T(T a, T b, const char *file, int line) { \
        if (b == 0) { fprintf(stderr, "%s:%d: division by zero\n", file, line); abort(); } \
        if ((T)-1 < 0 && b == (T)-1) return 0; \
        return (T)(a % b); \
    } \
    RIN_MAYBE_UNUSED static inline void rin_idiv_set_##T(T *a, T b, const char *file, int line) { *a = rin_idiv_##T(*a, b, file, line); } \
    RIN_MAYBE_UNUSED static inline void rin_irem_set_##T(T *a, T b, const char *file, int line) { *a = rin_irem_##T(*a, b, file, line); }
RIN_IDIV_DEFINE(i8, u8)
RIN_IDIV_DEFINE(i16, u16)
RIN_IDIV_DEFINE(i32, u32)
RIN_IDIV_DEFINE(i64, u64)
RIN_IDIV_DEFINE(u8, u8)
RIN_IDIV_DEFINE(u16, u16)
RIN_IDIV_DEFINE(u32, u32)
RIN_IDIV_DEFINE(u64, u64)
RIN_MAYBE_UNUSED static inline void rin_umul_set_u16(u16 *a, u16 b) { *a = (u16)((u32)*a * (u32)b); }

#define RIN_F2I_DEFINE(T, LOW, MAX, TOO_BIG) \
    RIN_MAYBE_UNUSED static inline T rin_f2i_##T(double x) { \
        if (x != x) return 0; \
        if (x >= (TOO_BIG)) return (T)(MAX); \
        if (x <= (double)(LOW)) return (T)(LOW); \
        return (T)x; \
    }
RIN_F2I_DEFINE(i8, -128, 127, 128.0)
RIN_F2I_DEFINE(i16, -32768, 32767, 32768.0)
RIN_F2I_DEFINE(i32, -2147483647 - 1, 2147483647, 2147483648.0)
RIN_F2I_DEFINE(i64, -9223372036854775807LL - 1, 9223372036854775807LL, 9223372036854775808.0)
RIN_F2I_DEFINE(u8, 0, 255, 256.0)
RIN_F2I_DEFINE(u16, 0, 65535, 65536.0)
RIN_F2I_DEFINE(u32, 0, 4294967295u, 4294967296.0)
RIN_F2I_DEFINE(u64, 0, 18446744073709551615ull, 18446744073709551616.0)

/* A proc that says it returns a value and reached its end without one. Reading what
   C leaves there is undefined, and an optimiser may run on into the next function. */
RIN_MAYBE_UNUSED static inline void rin_no_return(const char *file, int line) {
    fprintf(stderr, "%s:%d: this proc reached its end without returning a value\n", file, line);
    abort();
}

/* cstr_of found a terminator in an array of n chars at `from`. The copy lives until the block
   ends, and nothing else has its address. */
RIN_MAYBE_UNUSED static inline const c8 *rin_cstr_copy(c8 *to, const c8 *from, u64 n) {
    for (u64 i = 0; i < n; i++) {
        to[i] = from[i];
        if (from[i] == 0) break;
    }
    return to;
}

/* Where only the C compiler knows an integer's type: a generic's instances that differ, C's
   long, an enum. It picks by the type; the wrapping is through u64, which truncates back. */
#define RIN_ANY_INT(x, stem) _Generic((x), char: stem##_c, signed char: stem##_sc, short: stem##_s, int: stem##_i, \
    long: stem##_l, long long: stem##_ll, unsigned char: stem##_uc, unsigned short: stem##_us, unsigned: stem##_u, \
    unsigned long: stem##_ul, unsigned long long: stem##_ull)
#define RIN_ANY_NUM(x, stem) _Generic((x), char: stem##_c, signed char: stem##_sc, short: stem##_s, int: stem##_i, \
    long: stem##_l, long long: stem##_ll, unsigned char: stem##_uc, unsigned short: stem##_us, unsigned: stem##_u, \
    unsigned long: stem##_ul, unsigned long long: stem##_ull, float: stem##_f, double: stem##_d)
#define RIN_ANY_DEFINE_INT(T, tag, IS_SIGNED) \
    RIN_MAYBE_UNUSED static inline T rin_shl_##tag(T x, int n) { return (T)((u64)x << (n & (int)(sizeof(T) * 8 - 1))); } \
    RIN_MAYBE_UNUSED static inline T rin_shr_##tag(T x, int n) { return (T)(x >> (n & (int)(sizeof(T) * 8 - 1))); } \
    RIN_MAYBE_UNUSED static inline T rin_neg_##tag(T x) { return (T)((u64)0 - (u64)x); } \
    RIN_MAYBE_UNUSED static inline void rin_cadd_##tag(T *p, T v) { *p = (T)((u64)*p + (u64)v); } \
    RIN_MAYBE_UNUSED static inline void rin_csub_##tag(T *p, T v) { *p = (T)((u64)*p - (u64)v); } \
    RIN_MAYBE_UNUSED static inline void rin_cmul_##tag(T *p, T v) { *p = (T)((u64)*p * (u64)v); } \
    RIN_MAYBE_UNUSED static inline void rin_cshl_##tag(T *p, int n) { *p = rin_shl_##tag(*p, n); } \
    RIN_MAYBE_UNUSED static inline void rin_cshr_##tag(T *p, int n) { *p = rin_shr_##tag(*p, n); } \
    RIN_MAYBE_UNUSED static inline void rin_cdiv_##tag(T *p, T v, const char *file, int line) { \
        *p = IS_SIGNED ? (T)rin_idiv_i64((i64)*p, (i64)v, file, line) : (T)rin_idiv_u64((u64)*p, (u64)v, file, line); } \
    RIN_MAYBE_UNUSED static inline void rin_crem_##tag(T *p, T v, const char *file, int line) { \
        *p = IS_SIGNED ? (T)rin_irem_i64((i64)*p, (i64)v, file, line) : (T)rin_irem_u64((u64)*p, (u64)v, file, line); }
RIN_ANY_DEFINE_INT(char, c, ((char)-1 < 0))
RIN_ANY_DEFINE_INT(signed char, sc, 1)
RIN_ANY_DEFINE_INT(short, s, 1)
RIN_ANY_DEFINE_INT(int, i, 1)
RIN_ANY_DEFINE_INT(long, l, 1)
RIN_ANY_DEFINE_INT(long long, ll, 1)
RIN_ANY_DEFINE_INT(unsigned char, uc, 0)
RIN_ANY_DEFINE_INT(unsigned short, us, 0)
RIN_ANY_DEFINE_INT(unsigned, u, 0)
RIN_ANY_DEFINE_INT(unsigned long, ul, 0)
RIN_ANY_DEFINE_INT(unsigned long long, ull, 0)
#define RIN_ANY_DEFINE_FLOAT(T, tag) \
    RIN_MAYBE_UNUSED static inline T rin_neg_##tag(T x) { return -x; } \
    RIN_MAYBE_UNUSED static inline void rin_cadd_##tag(T *p, T v) { *p += v; } \
    RIN_MAYBE_UNUSED static inline void rin_csub_##tag(T *p, T v) { *p -= v; } \
    RIN_MAYBE_UNUSED static inline void rin_cmul_##tag(T *p, T v) { *p *= v; } \
    RIN_MAYBE_UNUSED static inline void rin_cdiv_##tag(T *p, T v, const char *file, int line) { (void)file; (void)line; *p /= v; }
RIN_ANY_DEFINE_FLOAT(float, f)
RIN_ANY_DEFINE_FLOAT(double, d)

/* The same for an expression whose type differs between a generic's instances. */
#define RIN_WRAP_OP(a, op, b) _Generic((a) op (b), \
    int: (int)((unsigned)(a) op (unsigned)(b)), \
    long: (long)((unsigned long)(a) op (unsigned long)(b)), \
    long long: (long long)((unsigned long long)(a) op (unsigned long long)(b)), \
    default: (a) op (b))
#define RIN_DIV_ANY(a, b, name) _Generic((a) / (b), \
    float: (a) / (b), double: (a) / (b), long double: (a) / (b), \
    int: rin_##name##_i32((i32)(a), (i32)(b), __FILE__, __LINE__), \
    unsigned: rin_##name##_u32((u32)(a), (u32)(b), __FILE__, __LINE__), \
    default: rin_##name##_i64((i64)(a), (i64)(b), __FILE__, __LINE__))
#define RIN_REM_ANY(a, b) _Generic((a) % (b), \
    int: rin_irem_i32((i32)(a), (i32)(b), __FILE__, __LINE__), \
    unsigned: rin_irem_u32((u32)(a), (u32)(b), __FILE__, __LINE__), \
    default: rin_irem_i64((i64)(a), (i64)(b), __FILE__, __LINE__))

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
#ifdef RIN_NO_BOUNDS_CHECKS
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
