# Verified 2026-10-24: clang --target=x86_64-pc-windows-msvc -c cv_loc_test.s -o t.obj; llvm-pdbutil dump -l t.obj
# shows "C:/devel/rasm/docs/sample.rasm ... 20 00000000 ! 21 00000005 !". See debugging.md, route C.
.intel_syntax noprefix
.text
.globl h
h:
.cv_file 1 "C:/devel/rasm/docs/sample.rasm"
.cv_func_id 0
.cv_loc 0 1 20 0
    mov eax, 1
.cv_loc 0 1 21 0
    add eax, 2
    ret
.Lh_end:
.section .debug$S,"dr"
.p2align 2
.long 4
.cv_linetable 0, h, .Lh_end
.cv_filechecksums
.cv_stringtable
