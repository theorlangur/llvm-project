rem cmake -G Ninja -DLLVM_ENABLE_PROJECTS="clang;lldb" -H.\llvm -Bbuildlldb
rmdir /S /Q buildlldb
rem cmake -G Ninja -DLLVM_ENABLE_PROJECTS="clang;lldb" -H.\llvm -Bbuildlldb -DCMAKE_BUILD_TYPE=Release
cmake -G Ninja -DLLVM_ENABLE_PROJECTS="clang;lldb" -H.\llvm -Bbuildlldb -DCMAKE_BUILD_TYPE=RelWithDebInfo
