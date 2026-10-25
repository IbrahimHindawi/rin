" Vim syntax file for rasm, the x64 assembler with rin's declarations
" Language:   rasm (*.rasm)
" Spec:       rasm/docs/spec.md; lexer in rin/src/asm.c (asm_lex)
"
" The mnemonic list between the GENERATED markers is written by
" editors/tools/gen_mnemonics.py from asm-lsp's x86_64.xml; edit the script,
" not the list.

if exists("b:current_syntax")
  finish
endif

let s:cpo_save = &cpo
set cpo&vim

syn case match
syn sync minlines=20

" ---------------------------------------------------------------------------
" Fallbacks first: when two matches start at the same column, the one defined
" later wins, and a keyword beats any match.

" ALL_CAPS names: #define constants, enum members, Win32 constants. A naming
" convention, not something rasm knows.
syn match rasmConstant "\<\u[A-Z0-9_]\+\>"

" `name:` at the start of a line followed by something: a global, or a field
" in a struct body.
syn match rasmDeclName "^\s*\zs\h\w*\ze\s*:\s*[^ \t;]"

" A parameter in a proc signature: `(a: T, b: *T)`.
syn match rasmParam "\%([(,]\s*\)\@<=\h\w*\ze\s*:"

" A type after `:`, `->`, `*`, `[N]`, `const`, or `alias =`. The primitive
" type keywords win over this; it colours struct, enum and alias names.
syn match rasmTypeRef "\%(\%(:\|->\|\<alias\%(\[[^\]]*\]\)\=\s*=\)\s*\%(\*\s*\|\[[^\]]*\]\s*\|\<const\>\s*\)*\)\@80<=\<\h\w*\>"

" The type in a typed path through a register: [rbx].body.id
syn match rasmPathType "\%(\%([[:alnum:]_\]]\)\@<!\[[^\]]*\]\.\)\@120<=\h\w*"

" COM vtables: ID3D11DeviceVtbl.CreateBuffer
syn match rasmVtblType "\<\h\w*Vtbl\>"
syn match rasmVtblMethod "\%(\<\h\w*Vtbl\.\)\@<=\h\w*"

" ---------------------------------------------------------------------------
" Declarations

syn match rasmProcName  "^\s*\zs\h\w*\ze\s*:\s*proc\>"
syn match rasmMacroName "^\s*\zs\h\w*\ze\s*:\s*macro\>"
syn match rasmTypeName  "^\s*\zs\h\w*\ze\s*:\s*\%(struct\|union\|enum\|alias\)\>"

" proc[external], enum[must_use], enum[u32], u32[align(16)]
syn match rasmAttribute "\%(\<\%(proc\|struct\|union\|enum\|alias\)\)\@<=\[[^\]]*\]" contains=rasmNumber
syn match rasmAttribute "\%(\w\)\@<=\[\s*\%(external\|external_emit\|static\|packed\|align\|callconv\|must_use\|no_layout_check\)\>[^\]]*\]" contains=rasmNumber

syn keyword rasmStructure struct union enum alias proc macro
syn keyword rasmStorage   const
syn keyword rasmInclude   import cinclude
syn keyword rasmBoolean   true false null

" Built-in types. `int` is left out: it is also a mnemonic, and a keyword
" would beat the mnemonic match. In a type position rasmTypeRef colours it.
syn keyword rasmType u8 u16 u32 u64 i8 i16 i32 i64 f32 f64 b8 b16 b32 b64 c8
syn keyword rasmType usize isize void char bool short long ulong float double
syn keyword rasmType intptr uintptr ptrdiff intmax uintmax

syn keyword rasmBuiltin sizeof alignof offsetof cast
syn keyword rasmCallMacro invoke vcall

" ---------------------------------------------------------------------------
" Instructions

" Registers are keywords in rasm, lowercase only, as in the lexer.
syn keyword rasmRegister rax rbx rcx rdx rsi rdi rbp rsp r8 r9 r10 r11 r12 r13 r14 r15
syn keyword rasmRegister eax ebx ecx edx esi edi ebp esp r8d r9d r10d r11d r12d r13d r14d r15d
syn keyword rasmRegister ax bx cx dx si di bp sp r8w r9w r10w r11w r12w r13w r14w r15w
syn keyword rasmRegister al bl cl dl sil dil bpl spl ah bh ch dh r8b r9b r10b r11b r12b r13b r14b r15b
syn keyword rasmRegister rip
syn match   rasmRegister "\<[xyz]mm\d\+\>"
syn match   rasmRegister "\<k\d\>"

syn keyword rasmSize byte word dword qword tbyte oword xmmword ymmword zmmword ptr

" The first word of a statement that is not a label, a declaration or a
" macro call is a mnemonic. Only known mnemonics are coloured, so a typo stands
" out; anchoring to the statement start keeps `out`, `in`, `test` and `loop`
" plain where they are names.
syn match rasmInstr "^\s*\zs\%(\%(rep\|repe\|repz\|repne\|repnz\|lock\)\s\+\)\=\h\w*\>\%(\s*[:(]\)\@!" contains=rasmMnemonic,rasmPrefix,rasmConstant
syn keyword rasmPrefix contained rep repe repz repne repnz lock

" BEGIN GENERATED MNEMONICS
" 1453 mnemonics; see editors/tools/gen_mnemonics.py
syn keyword rasmMnemonic contained aadd aand adc adcx add addpd addps addsd addss addsubpd addsubps
syn keyword rasmMnemonic contained adox aesdec aesdeclast aesenc aesenclast aesimc aeskeygenassist
syn keyword rasmMnemonic contained and andn andnpd andnps andpd andps aor axor bextr blendpd blendps
syn keyword rasmMnemonic contained blendvpd blendvps blsi blsmsk blsr bsf bsr bswap bt btc btr bts
syn keyword rasmMnemonic contained bzhi call cbw cdq cdqe clc cld cldemote clflush clflushopt cli
syn keyword rasmMnemonic contained clwb clzero cmc cmova cmovae cmovb cmovbe cmovc cmove cmovg
syn keyword rasmMnemonic contained cmovge cmovl cmovle cmovna cmovnae cmovnb cmovnbe cmovnc cmovne
syn keyword rasmMnemonic contained cmovng cmovnge cmovnl cmovnle cmovno cmovnp cmovns cmovnz cmovo
syn keyword rasmMnemonic contained cmovp cmovpe cmovpo cmovs cmovz cmp cmpbexadd cmpbxadd cmplexadd
syn keyword rasmMnemonic contained cmplxadd cmpnbexadd cmpnbxadd cmpnlexadd cmpnlxadd cmpnoxadd
syn keyword rasmMnemonic contained cmpnpxadd cmpnsxadd cmpnzxadd cmpoxadd cmppd cmpps cmppxadd cmpsb
syn keyword rasmMnemonic contained cmpsd cmpsq cmpss cmpsw cmpsxadd cmpxchg cmpxchg16b cmpxchg8b
syn keyword rasmMnemonic contained cmpzxadd comisd comiss cpuid cqo crc32 cvtdq2pd cvtdq2ps cvtpd2dq
syn keyword rasmMnemonic contained cvtpd2pi cvtpd2ps cvtpi2pd cvtpi2ps cvtps2dq cvtps2pd cvtps2pi
syn keyword rasmMnemonic contained cvtsd2si cvtsd2ss cvtsi2sd cvtsi2ss cvtss2sd cvtss2si cvttpd2dq
syn keyword rasmMnemonic contained cvttpd2pi cvttps2dq cvttps2pi cvttsd2si cvttss2si cwd cwde dec
syn keyword rasmMnemonic contained div divpd divps divsd divss dppd dpps emms endbr64 enter
syn keyword rasmMnemonic contained extractps extrq f2xm1 fabs fadd faddp fbld fbstp fchs fclex
syn keyword rasmMnemonic contained fcmovb fcmovbe fcmove fcmovnb fcmovnbe fcmovne fcmovnu fcmovu
syn keyword rasmMnemonic contained fcom fcomi fcomip fcomp fcompp fcos fdecstp fdiv fdivp fdivr
syn keyword rasmMnemonic contained fdivrp ffree fiadd ficom ficomp fidiv fidivr fild fimul fincstp
syn keyword rasmMnemonic contained finit fist fistp fisttp fisub fisubr fld fld1 fldcw fldenv fldl2e
syn keyword rasmMnemonic contained fldl2t fldlg2 fldln2 fldpi fldz fmul fmulp fnclex fninit fnop
syn keyword rasmMnemonic contained fnsave fnstcw fnstenv fnstsw fpatan fprem fprem1 fptan frndint
syn keyword rasmMnemonic contained frstor fsave fscale fsin fsincos fsqrt fst fstcw fstenv fstp
syn keyword rasmMnemonic contained fstsw fsub fsubp fsubr fsubrp ftst fucom fucomi fucomip fucomp
syn keyword rasmMnemonic contained fucompp fwait fxam fxch fxrstor fxrstor64 fxsave fxsave64 fxtract
syn keyword rasmMnemonic contained fyl2x fyl2xp1 gf2p8affineinvqb gf2p8affineqb gf2p8mulb haddpd
syn keyword rasmMnemonic contained haddps hlt hsubpd hsubps idiv imul in inc insb insd insertps
syn keyword rasmMnemonic contained insertq insw int int1 int3 invlpg iretq ja jae jb jbe jc je jecxz
syn keyword rasmMnemonic contained jg jge jl jle jmp jna jnae jnb jnbe jnc jne jng jnge jnl jnle jno
syn keyword rasmMnemonic contained jnp jns jnz jo jp jpe jpo jrcxz js jz kaddb kaddd kaddq kaddw
syn keyword rasmMnemonic contained kandb kandd kandnb kandnd kandnq kandnw kandq kandw kmovb kmovd
syn keyword rasmMnemonic contained kmovq kmovw knotb knotd knotq knotw korb kord korq kortestb
syn keyword rasmMnemonic contained kortestd kortestq kortestw korw kshiftlb kshiftld kshiftlq
syn keyword rasmMnemonic contained kshiftlw kshiftrb kshiftrd kshiftrq kshiftrw ktestb ktestd ktestq
syn keyword rasmMnemonic contained ktestw kunpckbw kunpckdq kunpckwd kxnorb kxnord kxnorq kxnorw
syn keyword rasmMnemonic contained kxorb kxord kxorq kxorw lahf lddqu ldmxcsr lea leave lfence lodsb
syn keyword rasmMnemonic contained lodsd lodsq lodsw loop loope loopne loopnz loopz lzcnt maskmovdqu
syn keyword rasmMnemonic contained maskmovq maxpd maxps maxsd maxss mcommit mfence minpd minps minsd
syn keyword rasmMnemonic contained minss monitor monitorx mov movabs movapd movaps movbe movd
syn keyword rasmMnemonic contained movddup movdir64b movdiri movdq2q movdqa movdqu movhlps movhpd
syn keyword rasmMnemonic contained movhps movlhps movlpd movlps movmskpd movmskps movntdq movntdqa
syn keyword rasmMnemonic contained movnti movntpd movntps movntq movntsd movntss movq movq2dq movsb
syn keyword rasmMnemonic contained movsd movshdup movsldup movsq movss movsw movsx movsxd movupd
syn keyword rasmMnemonic contained movups movzx mpsadbw mul mulpd mulps mulsd mulss mulx mwait
syn keyword rasmMnemonic contained mwaitx neg nop not or orpd orps out outsb outsd outsw pabsb pabsd
syn keyword rasmMnemonic contained pabsw packssdw packsswb packusdw packuswb paddb paddd paddq
syn keyword rasmMnemonic contained paddsb paddsw paddusb paddusw paddw palignr pand pandn pause
syn keyword rasmMnemonic contained pavgb pavgw pblendvb pblendw pclmulqdq pcmpeqb pcmpeqd pcmpeqq
syn keyword rasmMnemonic contained pcmpeqw pcmpestri pcmpestrm pcmpgtb pcmpgtd pcmpgtq pcmpgtw
syn keyword rasmMnemonic contained pcmpistri pcmpistrm pdep pext pextrb pextrd pextrq pextrw phaddd
syn keyword rasmMnemonic contained phaddsw phaddw phminposuw phsubd phsubsw phsubw pinsrb pinsrd
syn keyword rasmMnemonic contained pinsrq pinsrw pmaddubsw pmaddwd pmaxsb pmaxsd pmaxsw pmaxub
syn keyword rasmMnemonic contained pmaxud pmaxuw pminsb pminsd pminsw pminub pminud pminuw pmovmskb
syn keyword rasmMnemonic contained pmovsxbd pmovsxbq pmovsxbw pmovsxdq pmovsxwd pmovsxwq pmovzxbd
syn keyword rasmMnemonic contained pmovzxbq pmovzxbw pmovzxdq pmovzxwd pmovzxwq pmuldq pmulhrsw
syn keyword rasmMnemonic contained pmulhuw pmulhw pmulld pmullw pmuludq pop popcnt popf popfq por
syn keyword rasmMnemonic contained prefetch prefetchit0 prefetchit1 prefetchnta prefetcht0
syn keyword rasmMnemonic contained prefetcht1 prefetcht2 prefetchw psadbw pshufb pshufd pshufhw
syn keyword rasmMnemonic contained pshuflw pshufw psignb psignd psignw pslld pslldq psllq psllw
syn keyword rasmMnemonic contained psrad psraw psrld psrldq psrlq psrlw psubb psubd psubq psubsb
syn keyword rasmMnemonic contained psubsw psubusb psubusw psubw ptest punpckhbw punpckhdq punpckhqdq
syn keyword rasmMnemonic contained punpckhwd punpcklbw punpckldq punpcklqdq punpcklwd push pushf
syn keyword rasmMnemonic contained pushfq pxor rcl rcpps rcpss rcr rdfsbase rdgsbase rdmsr rdpid
syn keyword rasmMnemonic contained rdpmc rdpru rdrand rdseed rdtsc rdtscp ret retf retn rol ror rorx
syn keyword rasmMnemonic contained roundpd roundps roundsd roundss rsqrtps rsqrtss sahf sal sar sarx
syn keyword rasmMnemonic contained sbb scasb scasd scasq scasw serialize seta setae setb setbe setc
syn keyword rasmMnemonic contained sete setg setge setl setle setna setnae setnb setnbe setnc setne
syn keyword rasmMnemonic contained setng setnge setnl setnle setno setnp setns setnz seto setp setpe
syn keyword rasmMnemonic contained setpo sets setz sfence sha1msg1 sha1msg2 sha1nexte sha1rnds4
syn keyword rasmMnemonic contained sha256msg1 sha256msg2 sha256rnds2 shl shld shlx shr shrd shrx
syn keyword rasmMnemonic contained shufpd shufps sqrtpd sqrtps sqrtsd sqrtss stc std sti stmxcsr
syn keyword rasmMnemonic contained stosb stosd stosq stosw sub subpd subps subsd subss swapgs
syn keyword rasmMnemonic contained syscall sysenter sysexit sysret test tpause tzcnt ucomisd ucomiss
syn keyword rasmMnemonic contained ud0 ud1 ud2 umonitor umwait unpckhpd unpckhps unpcklpd unpcklps
syn keyword rasmMnemonic contained vaddpd vaddph vaddps vaddsd vaddsh vaddss vaddsubpd vaddsubps
syn keyword rasmMnemonic contained vaesdec vaesdeclast vaesenc vaesenclast vaesimc vaeskeygenassist
syn keyword rasmMnemonic contained valignd valignq vandnpd vandnps vandpd vandps vbcstnebf162ps
syn keyword rasmMnemonic contained vbcstnesh2ps vblendmpd vblendmps vblendpd vblendps vblendvpd
syn keyword rasmMnemonic contained vblendvps vbroadcastf128 vbroadcastf32x2 vbroadcastf32x4
syn keyword rasmMnemonic contained vbroadcastf32x8 vbroadcastf64x2 vbroadcastf64x4 vbroadcasti128
syn keyword rasmMnemonic contained vbroadcasti32x2 vbroadcasti32x4 vbroadcasti32x8 vbroadcasti64x2
syn keyword rasmMnemonic contained vbroadcasti64x4 vbroadcastsd vbroadcastss vcmppd vcmpph vcmpps
syn keyword rasmMnemonic contained vcmpsd vcmpsh vcmpss vcomisd vcomish vcomiss vcompresspd
syn keyword rasmMnemonic contained vcompressps vcvtdq2pd vcvtdq2ph vcvtdq2ps vcvtne2ps2bf16
syn keyword rasmMnemonic contained vcvtneebf162ps vcvtneeph2ps vcvtneobf162ps vcvtneoph2ps
syn keyword rasmMnemonic contained vcvtneps2bf16 vcvtpd2dq vcvtpd2ph vcvtpd2ps vcvtpd2qq vcvtpd2udq
syn keyword rasmMnemonic contained vcvtpd2uqq vcvtph2dq vcvtph2pd vcvtph2ps vcvtph2psx vcvtph2qq
syn keyword rasmMnemonic contained vcvtph2udq vcvtph2uqq vcvtph2uw vcvtph2w vcvtps2dq vcvtps2pd
syn keyword rasmMnemonic contained vcvtps2ph vcvtps2phx vcvtps2qq vcvtps2udq vcvtps2uqq vcvtqq2pd
syn keyword rasmMnemonic contained vcvtqq2ph vcvtqq2ps vcvtsd2sh vcvtsd2si vcvtsd2ss vcvtsd2usi
syn keyword rasmMnemonic contained vcvtsh2sd vcvtsh2si vcvtsh2ss vcvtsh2usi vcvtsi2sd vcvtsi2sh
syn keyword rasmMnemonic contained vcvtsi2ss vcvtss2sd vcvtss2sh vcvtss2si vcvtss2usi vcvttpd2dq
syn keyword rasmMnemonic contained vcvttpd2qq vcvttpd2udq vcvttpd2uqq vcvttph2dq vcvttph2qq
syn keyword rasmMnemonic contained vcvttph2udq vcvttph2uqq vcvttph2uw vcvttph2w vcvttps2dq
syn keyword rasmMnemonic contained vcvttps2qq vcvttps2udq vcvttps2uqq vcvttsd2si vcvttsd2usi
syn keyword rasmMnemonic contained vcvttsh2si vcvttsh2usi vcvttss2si vcvttss2usi vcvtudq2pd
syn keyword rasmMnemonic contained vcvtudq2ph vcvtudq2ps vcvtuqq2pd vcvtuqq2ph vcvtuqq2ps vcvtusi2sd
syn keyword rasmMnemonic contained vcvtusi2sh vcvtusi2ss vcvtuw2ph vcvtw2ph vdbpsadbw vdivpd vdivph
syn keyword rasmMnemonic contained vdivps vdivsd vdivsh vdivss vdpbf16ps vdppd vdpps vexpandpd
syn keyword rasmMnemonic contained vexpandps vextractf128 vextractf32x4 vextractf32x8 vextractf64x2
syn keyword rasmMnemonic contained vextractf64x4 vextracti128 vextracti32x4 vextracti32x8
syn keyword rasmMnemonic contained vextracti64x2 vextracti64x4 vextractps vfcmaddcph vfcmaddcsh
syn keyword rasmMnemonic contained vfcmulcph vfcmulcsh vfixupimmpd vfixupimmps vfixupimmsd
syn keyword rasmMnemonic contained vfixupimmss vfmadd132pd vfmadd132ph vfmadd132ps vfmadd132sd
syn keyword rasmMnemonic contained vfmadd132ss vfmadd213pd vfmadd213ph vfmadd213ps vfmadd213sd
syn keyword rasmMnemonic contained vfmadd213ss vfmadd231pd vfmadd231ph vfmadd231ps vfmadd231sd
syn keyword rasmMnemonic contained vfmadd231ss vfmaddcph vfmaddcsh vfmaddsub132pd vfmaddsub132ph
syn keyword rasmMnemonic contained vfmaddsub132ps vfmaddsub213pd vfmaddsub213ph vfmaddsub213ps
syn keyword rasmMnemonic contained vfmaddsub231pd vfmaddsub231ph vfmaddsub231ps vfmsub132pd
syn keyword rasmMnemonic contained vfmsub132ph vfmsub132ps vfmsub132sd vfmsub132sh vfmsub132ss
syn keyword rasmMnemonic contained vfmsub213pd vfmsub213ph vfmsub213ps vfmsub213sd vfmsub213sh
syn keyword rasmMnemonic contained vfmsub213ss vfmsub231pd vfmsub231ph vfmsub231ps vfmsub231sd
syn keyword rasmMnemonic contained vfmsub231sh vfmsub231ss vfmsubadd132pd vfmsubadd132ph
syn keyword rasmMnemonic contained vfmsubadd132ps vfmsubadd213pd vfmsubadd213ph vfmsubadd213ps
syn keyword rasmMnemonic contained vfmsubadd231pd vfmsubadd231ph vfmsubadd231ps vfmulcph vfmulcsh
syn keyword rasmMnemonic contained vfnmadd132pd vfnmadd132ph vfnmadd132ps vfnmadd132sd vfnmadd132ss
syn keyword rasmMnemonic contained vfnmadd213pd vfnmadd213ph vfnmadd213ps vfnmadd213sd vfnmadd213ss
syn keyword rasmMnemonic contained vfnmadd231pd vfnmadd231ph vfnmadd231ps vfnmadd231sd vfnmadd231ss
syn keyword rasmMnemonic contained vfnmsub132pd vfnmsub132ph vfnmsub132ps vfnmsub132sd vfnmsub132sh
syn keyword rasmMnemonic contained vfnmsub132ss vfnmsub213pd vfnmsub213ph vfnmsub213ps vfnmsub213sd
syn keyword rasmMnemonic contained vfnmsub213sh vfnmsub213ss vfnmsub231pd vfnmsub231ph vfnmsub231ps
syn keyword rasmMnemonic contained vfnmsub231sd vfnmsub231sh vfnmsub231ss vfpclasspd vfpclassph
syn keyword rasmMnemonic contained vfpclassps vfpclasssd vfpclasssh vfpclassss vgatherdpd vgatherdps
syn keyword rasmMnemonic contained vgatherqpd vgatherqps vgetexppd vgetexpph vgetexpps vgetexpsd
syn keyword rasmMnemonic contained vgetexpsh vgetexpss vgetmantpd vgetmantph vgetmantps vgetmantsd
syn keyword rasmMnemonic contained vgetmantsh vgetmantss vgf2p8affineinvqb vgf2p8affineqb vgf2p8mulb
syn keyword rasmMnemonic contained vhaddpd vhaddps vhsubpd vhsubps vinsertf128 vinsertf32x4
syn keyword rasmMnemonic contained vinsertf32x8 vinsertf64x2 vinsertf64x4 vinserti128 vinserti32x4
syn keyword rasmMnemonic contained vinserti32x8 vinserti64x2 vinserti64x4 vinsertps vlddqu vldmxcsr
syn keyword rasmMnemonic contained vmaskmovdqu vmaskmovpd vmaskmovps vmaxpd vmaxph vmaxps vmaxsd
syn keyword rasmMnemonic contained vmaxsh vmaxss vminpd vminph vminps vminsd vminsh vminss vmovapd
syn keyword rasmMnemonic contained vmovaps vmovd vmovddup vmovdqa vmovdqa32 vmovdqa64 vmovdqu
syn keyword rasmMnemonic contained vmovdqu16 vmovdqu32 vmovdqu64 vmovdqu8 vmovhlps vmovhpd vmovhps
syn keyword rasmMnemonic contained vmovlhps vmovlpd vmovlps vmovmskpd vmovmskps vmovntdq vmovntdqa
syn keyword rasmMnemonic contained vmovntpd vmovntps vmovq vmovsd vmovsh vmovshdup vmovsldup vmovss
syn keyword rasmMnemonic contained vmovupd vmovups vmovw vmpsadbw vmulpd vmulph vmulps vmulsd vmulsh
syn keyword rasmMnemonic contained vmulss vorpd vorps vpabsb vpabsd vpabsq vpabsw vpackssdw
syn keyword rasmMnemonic contained vpacksswb vpackusdw vpackuswb vpaddb vpaddd vpaddq vpaddsb
syn keyword rasmMnemonic contained vpaddsw vpaddusb vpaddusw vpaddw vpalignr vpand vpandd vpandn
syn keyword rasmMnemonic contained vpandnd vpandnq vpandq vpavgb vpavgw vpblendd vpblendmb vpblendmd
syn keyword rasmMnemonic contained vpblendmq vpblendmw vpblendvb vpblendw vpbroadcastb vpbroadcastd
syn keyword rasmMnemonic contained vpbroadcastmb2q vpbroadcastmw2d vpbroadcastq vpbroadcastw
syn keyword rasmMnemonic contained vpclmulqdq vpcmpb vpcmpd vpcmpeqb vpcmpeqd vpcmpeqq vpcmpeqw
syn keyword rasmMnemonic contained vpcmpestri vpcmpestrm vpcmpgtb vpcmpgtd vpcmpgtq vpcmpgtw
syn keyword rasmMnemonic contained vpcmpistri vpcmpistrm vpcmpq vpcmpub vpcmpud vpcmpuq vpcmpuw
syn keyword rasmMnemonic contained vpcmpw vpcompressb vpcompressd vpcompressq vpcompressw
syn keyword rasmMnemonic contained vpconflictd vpconflictq vpdpbssd vpdpbssds vpdpbsud vpdpbsuds
syn keyword rasmMnemonic contained vpdpbusd vpdpbusds vpdpbuud vpdpbuuds vpdpwssd vpdpwssds vpdpwsud
syn keyword rasmMnemonic contained vpdpwsuds vpdpwusd vpdpwusds vpdpwuud vpdpwuuds vperm2f128
syn keyword rasmMnemonic contained vperm2i128 vpermb vpermd vpermi2b vpermi2d vpermi2pd vpermi2ps
syn keyword rasmMnemonic contained vpermi2q vpermi2w vpermilpd vpermilps vpermpd vpermps vpermq
syn keyword rasmMnemonic contained vpermt2b vpermt2d vpermt2pd vpermt2ps vpermt2q vpermt2w vpermw
syn keyword rasmMnemonic contained vpexpandb vpexpandd vpexpandq vpexpandw vpextrb vpextrd vpextrq
syn keyword rasmMnemonic contained vpextrw vpgatherdd vpgatherdq vpgatherqd vpgatherqq vphaddd
syn keyword rasmMnemonic contained vphaddsw vphaddw vphminposuw vphsubd vphsubsw vphsubw vpinsrb
syn keyword rasmMnemonic contained vpinsrd vpinsrq vpinsrw vplzcntd vplzcntq vpmadd52huq vpmadd52luq
syn keyword rasmMnemonic contained vpmaddubsw vpmaddwd vpmaskmovd vpmaskmovq vpmaxsb vpmaxsd vpmaxsq
syn keyword rasmMnemonic contained vpmaxsw vpmaxub vpmaxud vpmaxuq vpmaxuw vpminsb vpminsd vpminsq
syn keyword rasmMnemonic contained vpminsw vpminub vpminud vpminuq vpminuw vpmovb2m vpmovd2m vpmovdb
syn keyword rasmMnemonic contained vpmovdw vpmovm2b vpmovm2d vpmovm2q vpmovm2w vpmovmskb vpmovq2m
syn keyword rasmMnemonic contained vpmovqb vpmovqd vpmovqw vpmovsdb vpmovsdw vpmovsqb vpmovsqd
syn keyword rasmMnemonic contained vpmovsqw vpmovswb vpmovsxbd vpmovsxbq vpmovsxbw vpmovsxdq
syn keyword rasmMnemonic contained vpmovsxwd vpmovsxwq vpmovusdb vpmovusdw vpmovusqb vpmovusqd
syn keyword rasmMnemonic contained vpmovusqw vpmovuswb vpmovw2m vpmovwb vpmovzxbd vpmovzxbq
syn keyword rasmMnemonic contained vpmovzxbw vpmovzxdq vpmovzxwd vpmovzxwq vpmuldq vpmulhrsw
syn keyword rasmMnemonic contained vpmulhuw vpmulhw vpmulld vpmullq vpmullw vpmultishiftqb vpmuludq
syn keyword rasmMnemonic contained vpopcntb vpopcntd vpopcntq vpopcntw vpor vpord vporq vprold
syn keyword rasmMnemonic contained vprolq vprolvd vprolvq vprord vprorq vprorvd vprorvq vpsadbw
syn keyword rasmMnemonic contained vpscatterdd vpscatterdq vpscatterqd vpscatterqq vpshldd vpshldq
syn keyword rasmMnemonic contained vpshldvd vpshldvq vpshldvw vpshldw vpshrdd vpshrdq vpshrdvd
syn keyword rasmMnemonic contained vpshrdvq vpshrdvw vpshrdw vpshufb vpshufbitqmb vpshufd vpshufhw
syn keyword rasmMnemonic contained vpshuflw vpsignb vpsignd vpsignw vpslld vpslldq vpsllq vpsllvd
syn keyword rasmMnemonic contained vpsllvq vpsllvw vpsllw vpsrad vpsraq vpsravd vpsravq vpsravw
syn keyword rasmMnemonic contained vpsraw vpsrld vpsrldq vpsrlq vpsrlvd vpsrlvq vpsrlvw vpsrlw
syn keyword rasmMnemonic contained vpsubb vpsubd vpsubq vpsubsb vpsubsw vpsubusb vpsubusw vpsubw
syn keyword rasmMnemonic contained vpternlogd vpternlogq vptest vptestmb vptestmd vptestmq vptestmw
syn keyword rasmMnemonic contained vptestnmb vptestnmd vptestnmq vptestnmw vpunpckhbw vpunpckhdq
syn keyword rasmMnemonic contained vpunpckhqdq vpunpckhwd vpunpcklbw vpunpckldq vpunpcklqdq
syn keyword rasmMnemonic contained vpunpcklwd vpxor vpxord vpxorq vrangepd vrangeps vrangesd
syn keyword rasmMnemonic contained vrangess vrcp14pd vrcp14ps vrcp14sd vrcp14ss vrcpph vrcpps vrcpsh
syn keyword rasmMnemonic contained vrcpss vreducepd vreduceph vreduceps vreducesd vreducesh
syn keyword rasmMnemonic contained vreducess vrndscalepd vrndscaleph vrndscaleps vrndscalesd
syn keyword rasmMnemonic contained vrndscalesh vrndscaless vroundpd vroundps vroundsd vroundss
syn keyword rasmMnemonic contained vrsqrt14pd vrsqrt14ps vrsqrt14sd vrsqrt14ss vrsqrtph vrsqrtps
syn keyword rasmMnemonic contained vrsqrtsh vrsqrtss vscalefpd vscalefph vscalefps vscalefsd
syn keyword rasmMnemonic contained vscalefsh vscalefss vscatterdpd vscatterdps vscatterqpd
syn keyword rasmMnemonic contained vscatterqps vsha512msg1 vsha512msg2 vsha512rnds2 vshuff32x4
syn keyword rasmMnemonic contained vshuff64x2 vshufi32x4 vshufi64x2 vshufpd vshufps vsm3msg1
syn keyword rasmMnemonic contained vsm3msg2 vsm3rnds2 vsm4key4 vsm4rnds4 vsqrtpd vsqrtph vsqrtps
syn keyword rasmMnemonic contained vsqrtsd vsqrtsh vsqrtss vstmxcsr vsubpd vsubph vsubps vsubsd
syn keyword rasmMnemonic contained vsubsh vsubss vtestpd vtestps vucomisd vucomish vucomiss
syn keyword rasmMnemonic contained vunpckhpd vunpckhps vunpcklpd vunpcklps vxorpd vxorps vzeroall
syn keyword rasmMnemonic contained vzeroupper wait wbinvd wrfsbase wrgsbase wrmsr xadd xchg xgetbv
syn keyword rasmMnemonic contained xlat xlatb xor xorpd xorps xrstor xrstor64 xrstors xsave xsave64
syn keyword rasmMnemonic contained xsavec xsaveopt xsaves xsetbv
" END GENERATED MNEMONICS

" A macro invocation: a statement that starts with `name(`.
syn match rasmMacroCall "^\s*\zs\h\w*\ze\s*("

" A label: `name:` alone on its line.
syn match rasmLabel "^\s*\zs\h\w*:\ze\s*\%(;.*\)\=$"

" The address-of suffix: window_proc&, c.transform&
syn match rasmAddrOf "\%([[:alnum:]_\]]\)\@<=&\ze\%(\s*[,)};\]]\|\s*$\)"
syn match rasmArrow "->"

" ---------------------------------------------------------------------------
" Literals

syn match rasmNumber "\%([[:alnum:]_.)\]]\)\@<!-\=\<\d[0-9_]*[uUlL]*\>"
syn match rasmNumber "\<0[xX][0-9A-Fa-f_]\+[uUlL]*\>"
syn match rasmNumber "\<0[bB][01_]\+[uUlL]*\>"
syn match rasmFloat  "\%([[:alnum:]_.)\]]\)\@<!-\=\<\d[0-9_]*\.\d\+\%([eE][+-]\=\d\+\)\=[fF]\=\>"
syn match rasmFloat  "\%([[:alnum:]_.)\]]\)\@<!-\=\<\d[0-9_]*[eE][+-]\=\d\+[fF]\=\>"

syn match  rasmEscape "\\\%([abfnrtv\\'\"?]\|x\x\+\|\o\{1,3}\)" contained
syn match  rasmFormat "%[-+ #0]*\%(\d\+\|\*\)\=\%(\.\%(\d\+\|\*\)\)\=\%(hh\|h\|ll\|l\|I64\|I32\|I\|z\|j\|t\|L\)\=[diouxXcsSpfFeEgGaA%]" contained
syn region rasmString start=+"+ skip=+\\\\\|\\"+ end=+"+ end=+$+ oneline contains=rasmEscape,rasmFormat
syn match  rasmChar "'\%(\\\%(x\x\+\|\o\{1,3}\|.\)\|[^'\\]\)'" contains=rasmEscape

" ---------------------------------------------------------------------------
" #define and comments, last so they win.

" `#` is C's preprocessor. An object-like #define is the only # line a .rasm
" file accepts. It runs to the end of the line, and a `;` in it is an error
" (C would read it as part of the value), so it is not a comment there.
syn match  rasmPreProcError "^\s*#.*$"
syn region rasmDefine start="^\s*#\s*define\>" end="$" keepend contains=rasmDefineKw,rasmDefineName,rasmDefineSemi,rasmNumber,rasmFloat,rasmString,rasmChar,rasmConstant,rasmBuiltin,rasmType
syn match  rasmDefineKw "^\s*#\s*define\>" contained
syn match  rasmDefineName "\%(#\s*define\s\+\)\@<=\h\w*" contained
syn match  rasmDefineSemi ";.*$" contained

syn keyword rasmTodo TODO FIXME XXX NOTE HACK INTERESTING contained
syn match   rasmComment ";.*$" contains=rasmTodo,@Spell

" ---------------------------------------------------------------------------

hi def link rasmComment      Comment
hi def link rasmTodo         Todo
hi def link rasmString       String
hi def link rasmChar         Character
hi def link rasmEscape       SpecialChar
hi def link rasmFormat       SpecialChar
hi def link rasmNumber       Number
hi def link rasmFloat        Float
hi def link rasmBoolean      Boolean
hi def link rasmConstant     Constant

hi def link rasmStructure    Structure
hi def link rasmStorage      StorageClass
hi def link rasmInclude      Include
hi def link rasmDefine       PreProc
hi def link rasmDefineKw     Define
hi def link rasmDefineName   Macro
hi def link rasmDefineSemi   Error
hi def link rasmPreProcError Error
hi def link rasmAttribute    PreProc

hi def link rasmType         Type
hi def link rasmTypeRef      Type
hi def link rasmPathType     Type
hi def link rasmVtblType     Type
hi def link rasmTypeName     Typedef
hi def link rasmProcName     Function
hi def link rasmMacroName    Macro
hi def link rasmDeclName     Identifier
hi def link rasmParam        Identifier
hi def link rasmVtblMethod   Function

hi def link rasmMnemonic     Statement
hi def link rasmPrefix       Statement
hi def link rasmRegister     Special
hi def link rasmSize         Type
hi def link rasmLabel        Label
hi def link rasmMacroCall    Macro
hi def link rasmCallMacro    Macro
hi def link rasmBuiltin      Function
hi def link rasmAddrOf       Operator
hi def link rasmArrow        Operator

let b:current_syntax = "rasm"

let &cpo = s:cpo_save
unlet s:cpo_save
