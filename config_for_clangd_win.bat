rem cmake -Hllvm -Bbuildwin -A x64 -T ClangCL -DLLVM_ENABLE_PROJECTS="clang;clang-tools-extra" -DLLVM_TARGETS_TO_BUILD="X86" -DLLVM_USE_LINKER=lld -DLLVM_ENABLE_LTO=Full
rem cmake -Hllvm -Bbuildwin_msvc2 -A x64 -DLLVM_ENABLE_PROJECTS="clang;clang-tools-extra" -DLLVM_TARGETS_TO_BUILD="X86"
rem cmake -Hllvm -Bbuildwin_msvc_dbg -A x64 -DLLVM_ENABLE_PROJECTS="clang;clang-tools-extra" -DLLVM_TARGETS_TO_BUILD="X86"
rem cmake -Hllvm -Bbuildwin_mid2024 -A x64 -DLLVM_ENABLE_PROJECTS="clang;clang-tools-extra" -DLLVM_TARGETS_TO_BUILD="X86"
rem cmake -G Ninja -Hllvm -Bbuildwin_clang_mid2024 -DLLVM_ENABLE_PROJECTS="clang;clang-tools-extra" -DLLVM_TARGETS_TO_BUILD="X86" -DCMAKE_C_COMPILER=d:\Developing\cppindex\tmp\llvm\llvm-18.1.8\bin\clang.exe -DCMAKE_CXX_COMPILER=d:\Developing\cppindex\tmp\llvm\llvm-18.1.8\bin\clang++.exe -DCMAKE_ASM_COMPILER=d:\Developing\cppindex\tmp\llvm\llvm-18.1.8\bin\llvm-as.exe
cmake -Hllvm -A x64 -TClangCL -Bbuildwin_clang_mid2024 -DLLVM_ENABLE_PROJECTS="clang;clang-tools-extra" -DLLVM_TARGETS_TO_BUILD="X86" -DLLVM_ENABLE_LTO=Full
