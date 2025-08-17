#!/bin/sh
cmake -Hllvm -Bbuild \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-march=x86-64-v3" \
    -DCMAKE_C_FLAGS="-march=x86-64-v3" \
    -DLLVM_ENABLE_PROJECTS="clang;clang-tools-extra" \
    -DLLVM_TARGETS_TO_BUILD="X86" \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DLLVM_USE_LINKER=lld \
    -DLLVM_ENABLE_LTO=Full
