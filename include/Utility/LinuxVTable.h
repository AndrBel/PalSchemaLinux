#pragma once
// Linux-Portierung: PalSchemas AOB-Signaturen stammen aus der Windows-Binary und
// koennen auf dem Linux-Build prinzipiell nicht passen (anderer Compiler, anderer
// Maschinencode).
//
// Der Linux-Server braucht sie aber gar nicht: PalServer-Linux-Shipping ist als
// EXEC (nicht PIE) gebaut und exportiert ~30.000 vtable-Symbole in .dynsym.
// Virtuelle Methoden sind damit ueber dlsym + Slot-Index direkt adressierbar.
//
// Slot-Zaehlung folgt dem Itanium-ABI: Index 0 = offset-to-top, 1 = typeinfo,
// ab 2 die Funktionszeiger.
#include <dlfcn.h>
#include <cstdint>

namespace PS::LinuxVTable {

    inline void** GetVTable(const char* mangledVTableSymbol)
    {
        return reinterpret_cast<void**>(dlsym(RTLD_DEFAULT, mangledVTableSymbol));
    }

    inline void* GetVirtualFunction(const char* mangledVTableSymbol, int slot)
    {
        auto** vtable = GetVTable(mangledVTableSymbol);
        if (!vtable) return nullptr;
        return vtable[slot];
    }
}
