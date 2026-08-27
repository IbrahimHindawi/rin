#include <stdio.h>
#include <string.h>

#define CORE_IMPL
#include <core.h>

#include "saha.h"

typedef struct Payload Payload;
struct Payload {
    i32 id;
    i32 mx;
    char *str;
};

typedef struct vec4i8 vec4i8;
struct vec4i8 { i8 x; i8 y; i8 z; i8 w; };

void Arena_test() {
    Arena arena = {0};
    arenaInit(&arena);

    i32 len = 4;
    f32 *nums = arenaPushArray(&arena, i32, len);
    for (i32 i = 0; i < len; ++i) {
        nums[i] = (f32)(i + 1);
    }
    for (i32 i = 0; i < len; ++i) {
        printf("%f ", nums[i]);
    }
    printf("\n");

    u8 *ptr = (u8 *)nums;
    for (i32 i = 0; i < sizeof(f32) * len; ++i) {
        printf("%02x ", ptr[i]);
    }
    printf("\n");

    // void *old_ptr = nums;
    // nums = arenaPushArray(&arena, i32, len * 2);
    // memcpy(nums, old_ptr, sizeof(i32) * len);
    nums = arenaRealloc(&arena, i32, 8, nums, 4);
    for (i32 i = 0; i < 8; ++i) {
        nums[i] = (f32)(i + 1);
    }

    void *pos = arena.cursor;

    char *str0 = strAlloc(&arena, "this is a te");
    char *str1 = strAlloc(&arena, "st string to");
    char *str2 = strAlloc(&arena, "alloc bytes.");
    printf("%s\n", str0);
    printf("%s\n", str1);
    printf("%s\n", str2);

    strDealloc(&arena, str2);
    str2 = strAlloc(&arena, "fortitude");
    printf("%s\n", str0);
    printf("%s\n", str1);
    printf("%s\n", str2);

    arenaSetPos(&arena, pos);
    i8 *x = arenaPush(&arena, sizeof(i8), _Alignof(i8));
    *x = 0xDD;

    Payload *pld = arenaPushStruct(&arena, Payload);
    pld->id = 0xDEADBEEF;
    pld->mx = 0xCAFEBABE;
    pld->str = "Name0";
    arenaPop(&arena, sizeof(Payload));

    pld = arenaPushStruct(&arena, Payload);
    pld->id = 0xFFFFFFFF;
    pld->mx = 0xFFFFFFFF;
    pld->str = "Name0";
    arenaPop(&arena, sizeof(Payload));
    pld->id = 0xDEADBEEF;
    pld->mx = 0xCAFEBABE;
    pld->str = "Name0";
    arenaSetPos(&arena, pos);

    pos = arena.cursor;
    nums = arenaPushArray(&arena, i32, len);
    for (i32 i = 0; i < len; ++i) {
        nums[i] = (f32)(i + 1);
    }
    arenaSetPos(&arena, pos);

    arenaClear(&arena);

    // vec4i8 *vs = arenaPushArrayZero(&arena, vec4i8, 32);
    const i32 npts = 32;
    vec4i8 *vs = arenaPushArray(&arena, vec4i8, npts);
    for (i32 i = 0; i < npts; ++i) {
        vs[i].x = 0xAA;
        vs[i].y = 0xBB;
        vs[i].z = 0xCC;
        vs[i].w = 0xDD;
    }
    arenaPopArray(&arena, vec4i8, npts);

    arenaPrint(&arena);
    arenaClear(&arena);
}

i32 main(i32 argc, char *argv[]) {
    printf("saha test begin.\n");
    Arena_test();
    printf("saha test end.\n");
    return 0;
}
