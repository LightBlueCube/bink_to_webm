## Build (MSYS2)

```sh
pacman -Sy --needed make patch mingw-w64-x86_64-{gcc,cmake,pkgconf,MinHook,nasm}

cmake -S . -B build -G "MinGW Makefiles"
cmake --build build -j
```
