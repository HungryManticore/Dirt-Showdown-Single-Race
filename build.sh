#!/bin/sh
# Builds release/SplitToSingle.asi with clang + lld (no MSVC / Windows SDK needed).
set -e
cd "$(dirname "$0")"
mkdir -p build release
for l in kernel32 user32 gdi32; do
    llvm-dlltool -m i386 -k -d src/$l.def -l build/$l.lib
done
F="--target=i686-pc-windows-msvc -O2 -ffreestanding -fno-builtin -fno-stack-protector -fms-extensions -Wall -Wno-unused-function"
clang $F -c src/SplitToSingle.c -o build/SplitToSingle.obj
clang $F -c src/overlay.c      -o build/overlay.obj
lld-link /dll /nodefaultlib /entry:DllMain /subsystem:windows /machine:x86 /safeseh:no \
    /out:release/SplitToSingle.asi build/SplitToSingle.obj build/overlay.obj \
    build/kernel32.lib build/user32.lib build/gdi32.lib
echo "built release/SplitToSingle.asi"
