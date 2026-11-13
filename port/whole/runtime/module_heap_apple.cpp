// Mach-O stand-in for the ELF linker's --wrap=_Znwm (and _Znam, _ZdlPv, _ZdaPv) that the REL
// modules are linked with on Linux and Android: their `new` / `delete` must use the game heap
// (runtime/game_heap.cpp in libre4_main), not the process-wide C++ runtime. Each module carries
// these definitions; two-level namespaces make the module's own references bind to them.
#include <stddef.h>

extern "C" {
void* __wrap__Znwj(size_t n);
void* __wrap__Znaj(size_t n);
void* __wrap__Znwm(size_t n);
void* __wrap__Znam(size_t n);
void __wrap__ZdlPv(void* p);
void __wrap__ZdaPv(void* p);
}

void* operator new(size_t n)
{
    return __wrap__Znwm(n);
}
void* operator new[](size_t n)
{
    return __wrap__Znam(n);
}
void operator delete(void* p) noexcept
{
    __wrap__ZdlPv(p);
}
void operator delete[](void* p) noexcept
{
    __wrap__ZdaPv(p);
}
