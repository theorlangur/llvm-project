#!/bin/sh
cmake -Hllvm -Bbuild_asan \
    -DCMAKE_CXX_FLAGS="-fsanitize=address -march=x86-64-v3" \
    -DCMAKE_C_FLAGS="-fsanitize=address -march=x86-64-v3" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DLLVM_ENABLE_PROJECTS="clang;clang-tools-extra" \
    -DLLVM_TARGETS_TO_BUILD="X86" \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DLLVM_USE_LINKER=lld

