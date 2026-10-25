# Builds proto.exe the way rasm would: code from code.s (clang's assembler,
# .cv_loc lines pointing at proto.rasm, an S_GPROC32 for main), typed data
# from data.c (clang-cl /Z7; in rasm this would be the data declarations
# compiled through rin's C backend). Verified 2026-10-24 with LLVM 22 and lldb.
$ll = "C:\Program Files\LLVM\bin"
Set-Location $PSScriptRoot
& "$ll\clang.exe" --target=x86_64-pc-windows-msvc -c code.s -o code.obj
& "$ll\clang-cl.exe" -nologo /c /Z7 data.c /Fodata.obj
$k = (Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\Lib\*\um\x64" | Select-Object -Last 1).FullName
& "$ll\lld-link.exe" code.obj data.obj /debug:full /entry:main /subsystem:console "/libpath:$k" kernel32.lib /out:proto.exe /pdb:proto.pdb

# Visual Studio:  devenv /debugexe proto.exe
#   open proto.rasm, F9 on line 6, F5, then F10; Watch: desc
# lldb (needs a python311.dll on PATH and PYTHONHOME):
#   lldb -b -s cmds.lldb proto.exe
