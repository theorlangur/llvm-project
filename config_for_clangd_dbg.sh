#!/bin/sh
cmake -Hllvm -Bbuild_dbg \
    -DCMAKE_CXX_FLAGS="-Og" \
    -DCMAKE_C_FLAGS="-Og" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DLLVM_ENABLE_PROJECTS="clang;clang-tools-extra" \
    -DLLVM_TARGETS_TO_BUILD="X86" \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DLLVM_USE_LINKER=lld

