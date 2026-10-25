.intel_syntax noprefix
.text
.globl main
.p2align 4
main:
.cv_file 1 "C:/devel/rasm/docs/debug-proto/proto.rasm"
.cv_func_id 0
.cv_loc 0 1 5 0
    sub rsp, 40
.cv_loc 0 1 6 0
    mov eax, dword ptr [rip + desc]
.cv_loc 0 1 7 0
    add eax, 1
.cv_loc 0 1 8 0
    mov dword ptr [rip + desc], eax
.cv_loc 0 1 9 0
    movss xmm0, dword ptr [rip + desc + 8]
.cv_loc 0 1 10 0
    addss xmm0, xmm0
.cv_loc 0 1 11 0
    movss dword ptr [rip + desc + 8], xmm0
.cv_loc 0 1 12 0
    xor ecx, ecx
.cv_loc 0 1 13 0
    call ExitProcess
.Lmain_end:

.section .debug$S,"dr"
.p2align 2
.long 4
.long 241
.long .Lsym_end - .Lsym_begin
.Lsym_begin:
.short .Lproc_end - .Lproc_begin
.Lproc_begin:
.short 0x1110                 # S_GPROC32
.long 0, 0, 0
.long .Lmain_end - main       # code size
.long 0
.long .Lmain_end - main
.long 0                       # no type
.secrel32 main
.secidx main
.byte 0
.asciz "main"
.p2align 2
.Lproc_end:
.short 2
.short 6                      # S_END
.Lsym_end:
.p2align 2
.cv_linetable 0, main, .Lmain_end
.cv_filechecksums
.cv_stringtable
