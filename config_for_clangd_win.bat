cmake -Hllvm -Bbuildwin -A x64 -T ClangCL -DLLVM_ENABLE_PROJECTS="clang;clang-tools-extra" -DLLVM_TARGETS_TO_BUILD="X86" -DLLVM_USE_LINKER=lld -DLLVM_ENABLE_LTO=Full
