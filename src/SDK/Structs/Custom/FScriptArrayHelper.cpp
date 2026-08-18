#include "SDK/Structs/Custom/FScriptArrayHelper.h"
#include "Unreal/FMemory.hpp"
#include "Unreal/Core/HAL/MemoryBase.hpp"
#include "Utility/Logging.h"
#include "Utility/PsTrace.h"
#include <cstdlib>
#include <cstdio>
#include <vector>
#include <dlfcn.h>

using namespace RC;
using namespace RC::Unreal;

namespace UECustom {
#ifdef __linux__
namespace {
    // Abbild des Speicherlayouts von TScriptArray: die Allokator-Basisklasse haelt den
    // Datenzeiger, danach folgen ArrayNum und ArrayMax (beide protected, deshalb hier
    // gespiegelt statt geerbt). Wird vor jeder Nutzung gegen die oeffentlichen Accessoren
    // geprueft -- stimmt es nicht, faellt der Code auf den urspruenglichen Weg zurueck.
    struct PsRawScriptArray {
        void* Data;
        RC::Unreal::int32 ArrayNum;
        RC::Unreal::int32 ArrayMax;
    };

    bool PsEngineArrayGrowEnabled()
    {
        static const bool Enabled = ::getenv("PS_ENGINE_ARRAY_GROW") != nullptr;
        return Enabled;
    }

    // UE4SS' Unreal::GMalloc ist unter Linux unbrauchbar, gleich doppelt:
    //   1. Die Heuristik in UE4SSProgram.cpp speichert eine Indirektionsstufe zu hoch
    //      (FMalloc*** statt FMalloc**), sodass (*GMalloc)->Fn() den Instanzzeiger als
    //      vtable liest und in Heap-Daten springt.
    //   2. Ihr Filter ("dritte virtuelle Funktion ist xor eax,eax; ret") trifft auf dieser
    //      Binary 71 Objekte, darunter Delegates; genommen wird das erste gefundene.
    // Der manuelle Override aus UE4SS_Addresses.ini hilft nicht, weil der Linux-Port die
    // gesamte Scan-Phase ueberspringt ("PS scan skipped (all overrides set, Linux)").
    //
    // Deshalb hier selbst bestimmen, und zwar symbolbasiert statt ueber eine feste Adresse:
    // Die Executable exportiert die vtables aller FMalloc-Klassen. Im Itanium-ABI steht im
    // Objekt der vptr = Symboladresse + 2*sizeof(void*) (offset-to-top + typeinfo). Damit
    // laesst sich der echte Allokator eindeutig identifizieren, statt ihn zu raten.
    struct PsMapRegion { uintptr_t Start; uintptr_t End; bool Readable; bool Writable; };

    const std::vector<PsMapRegion>& PsGetMaps()
    {
        static const std::vector<PsMapRegion> Regions = []{
            std::vector<PsMapRegion> Out;
            if (FILE* f = ::fopen("/proc/self/maps", "r"))
            {
                char line[512];
                while (::fgets(line, sizeof(line), f))
                {
                    unsigned long long a = 0, b = 0;
                    char perms[8] = {0};
                    if (::sscanf(line, "%llx-%llx %7s", &a, &b, perms) == 3)
                    {
                        Out.push_back(PsMapRegion{ static_cast<uintptr_t>(a),
                                                   static_cast<uintptr_t>(b),
                                                   perms[0] == 'r',
                                                   perms[1] == 'w' });
                    }
                }
                ::fclose(f);
            }
            return Out;
        }();
        return Regions;
    }

    bool PsIsReadable(uintptr_t Addr)
    {
        for (const auto& R : PsGetMaps())
        {
            if (Addr >= R.Start && Addr + sizeof(void*) <= R.End) return R.Readable;
        }
        return false;
    }

    RC::Unreal::FMalloc* PsResolveEngineMalloc()
    {
        static RC::Unreal::FMalloc* Cached = []() -> RC::Unreal::FMalloc* {
            static const char* Symbols[] = {
                "_ZTV14FMallocBinned2", "_ZTV14FMallocBinned3", "_ZTV13FMallocBinned",
                "_ZTV18FMallocBinnedArena", "_ZTV15FMallocMimalloc", "_ZTV15FMallocJemalloc",
            };
            std::vector<uintptr_t> Vptrs;
            for (const char* Sym : Symbols)
            {
                if (void* P = ::dlsym(RTLD_DEFAULT, Sym))
                {
                    Vptrs.push_back(reinterpret_cast<uintptr_t>(P) + 2 * sizeof(void*));
                }
            }
            PS::Trace("RESOLVE: %d vtable-Symbole", (int)Vptrs.size());
            if (Vptrs.empty()) return nullptr;

            // Zuerst die dokumentierte Adresse von &GMalloc pruefen. Sie stammt aus der
            // Analyse am laufenden Prozess und ist bei einer nicht-PIE-Binary stabil.
            // Sie wird NICHT blind benutzt, sondern gegen die vtable-Symbole verifiziert --
            // stimmt sie nach einem Spiel-Update nicht mehr, greift der Scan darunter.
            {
                const uintptr_t Known = 0xc055528ull;
                PS::Trace("RESOLVE: bekannte Adresse pruefen");
                if (PsIsReadable(Known))
                {
                    const uintptr_t Instance = *reinterpret_cast<uintptr_t*>(Known);
                    if (Instance > 0x10000 && PsIsReadable(Instance))
                    {
                        const uintptr_t Vptr = *reinterpret_cast<uintptr_t*>(Instance);
                        for (uintptr_t Expected : Vptrs)
                        {
                            if (Vptr == Expected)
                            {
                                PS::Trace("RESOLVE: bekannte Adresse passt, Instanz=%p", (void*)Instance);
                                PS::Log<RC::LogLevel::Normal>(
                                    STR("Engine-Allokator ueber bekannte Adresse: Instanz={}\n"),
                                    reinterpret_cast<void*>(Instance));
                                return reinterpret_cast<RC::Unreal::FMalloc*>(Instance);
                            }
                        }
                    }
                }
                PS::Trace("RESOLVE: bekannte Adresse passt nicht, scanne");
            }

            // Nur die beschreibbaren Bereiche der nicht-PIE-Executable durchsuchen
            // (niedrige Adressen), nicht den gesamten Prozessspeicher.
            for (const auto& R : PsGetMaps())
            {
                if (!R.Writable || !R.Readable || R.Start >= 0x10000000ull) continue;
                for (uintptr_t Addr = R.Start; Addr + sizeof(void*) <= R.End; Addr += sizeof(void*))
                {
                    const uintptr_t Instance = *reinterpret_cast<uintptr_t*>(Addr);
                    if (Instance < 0x10000 || !PsIsReadable(Instance)) continue;
                    const uintptr_t Vptr = *reinterpret_cast<uintptr_t*>(Instance);
                    for (uintptr_t Expected : Vptrs)
                    {
                        if (Vptr == Expected)
                        {
                            PS::Log<RC::LogLevel::Normal>(
                                STR("Engine-Allokator gefunden: &GMalloc={} Instanz={} vptr={}\n"),
                                reinterpret_cast<void*>(Addr),
                                reinterpret_cast<void*>(Instance),
                                reinterpret_cast<void*>(Vptr));
                            return reinterpret_cast<RC::Unreal::FMalloc*>(Instance);
                        }
                    }
                }
            }
            PS::Log<RC::LogLevel::Error>(STR("Engine-Allokator nicht gefunden.\n"));
            return nullptr;
        }();
        return Cached;
    }
}
#endif
    FScriptArrayHelper::FScriptArrayHelper(void* InScriptArray, RC::Unreal::FArrayProperty* InArrayProperty)
    {
        ScriptArray = static_cast<FScriptArray*>(InScriptArray);
        ArrayProperty = InArrayProperty;
    }

    FScriptArrayHelper::FScriptArrayHelper(FScriptArray* InScriptArray, FArrayProperty* InArrayProperty)
    {
        ScriptArray = InScriptArray;
        ArrayProperty = InArrayProperty;
    }

    void FScriptArrayHelper::Add(void* Value)
    {
        PS::Trace("      ADD: GetElementSize");
        auto ElementSize = GetElementSize();
        PS::Trace("      ADD: size=%d, GetMinAlignment", (int)ElementSize);
        auto ElementAlignment = GetMinAlignment();
        PS::Trace("      ADD: align=%d, GetInner", (int)ElementAlignment);
        auto InnerProperty = GetInner();
        PS::Trace("      ADD: inner=%p", (void*)InnerProperty);

#ifdef __linux__
        // Das Array gehoert der Engine: sein Puffer stammt von GMalloc. ScriptArray->Add()
        // laesst ihn ueber FMemory::Realloc wachsen, und das geht unter Linux auf ::realloc --
        // glibc bricht dann mit "realloc(): invalid pointer" ab (SIGABRT). Nachgewiesen an
        // DT_ItemShopCreateData / productDataArray.Items.
        //
        // Deshalb das Wachstum hier selbst erledigen, ueber FMemory::ReallocExternal, das
        // ueber (*GMalloc)->Realloc geht. Der Puffer bleibt damit durchgehend beim
        // Engine-Allokator -- kein globaler Umbau des Allokators noetig, der sich bereits
        // zweimal als nicht tragfaehig erwiesen hat (siehe palschema-linux-portierung.md).
        //
        // Wichtig: Diese Stelle laeuft auf dem Game-Thread. Genau darauf zielt die Warnung
        // im UE4SS-Linux-Port ab, dass FMallocBinned2 auf UE4SS' Hintergrund-Thread wegen
        // nicht initialisierter TLS-Caches abstuerzt -- hier trifft sie nicht zu.
        if (PsEngineArrayGrowEnabled())
        {
            PS::Trace("      ADD: Layout pruefen");
            auto* Raw = reinterpret_cast<PsRawScriptArray*>(ScriptArray);
            const bool LayoutMatches =
                Raw->Data == ScriptArray->GetData() &&
                Raw->ArrayNum == ScriptArray->Num() &&
                Raw->ArrayMax == ScriptArray->Max();

            if (LayoutMatches)
            {
                PS::Trace("      ADD: Layout ok, num=%d max=%d", (int)Raw->ArrayNum, (int)Raw->ArrayMax);
                const int32 OldNum = Raw->ArrayNum;
                if (OldNum + 1 > Raw->ArrayMax)
                {
                    // Verdoppeln wie UE, mindestens vier Elemente.
                    const int32 NewMax = OldNum > 0 ? OldNum * 2 : 4;
                    // FMemory::ReallocExternal waere die passende Funktion, ist aber private.
                    // GMalloc selbst ist oeffentlich deklariert (Unreal/FMemory.hpp), also
                    // direkt darueber gehen -- dasselbe Ziel, ohne UE4SS anzufassen.
                    PS::Trace("      ADD: Allokator aufloesen");
                    auto* EngineMalloc = PsResolveEngineMalloc();
                    PS::Trace("      ADD: Allokator=%p", (void*)EngineMalloc);
                    if (!EngineMalloc)
                    {
                        PS::Log<RC::LogLevel::Error>(STR("FScriptArrayHelper::Add: GMalloc nicht verfuegbar.\n"));
                        return;
                    }
                    const SIZE_T NewBytes = static_cast<SIZE_T>(NewMax) * static_cast<SIZE_T>(ElementSize);
                    // Realloc(nullptr, n) verhaelt sich bei UE-Allokatoren wie Malloc, aber
                    // darauf verlassen wir uns nicht -- der Zweig steht explizit da.
                    void* NewData = Raw->Data
                        ? EngineMalloc->Realloc(Raw->Data, NewBytes, static_cast<uint32>(ElementAlignment))
                        : EngineMalloc->Malloc(NewBytes, static_cast<uint32>(ElementAlignment));
                    if (!NewData)
                    {
                        PS::Log<RC::LogLevel::Error>(STR("FScriptArrayHelper::Add: ReallocExternal lieferte nullptr.\n"));
                        return;
                    }
                    Raw->Data = NewData;
                    Raw->ArrayMax = NewMax;
                }
                // Element zuerst fertigstellen, dann erst sichtbar machen: liefe waehrenddessen
                // eine Garbage Collection, laese der Collector sonst uninitialisierten Speicher.
                void* NewElement = static_cast<uint8*>(Raw->Data) + static_cast<size_t>(OldNum) * ElementSize;
                // Kein virtueller Aufruf: FProperty::InitializeValueInternal hat unter Linux
                // keinen verifizierten vtable-Offset. Der Standard (MSVC) sagt 0x150 -- genau
                // den Offset belegt hier aber das individuell verifizierte GetMinAlignment.
                // FProperty::InitializeValue nullt bei gesetztem CPF_ZeroConstructor ohnehin nur,
                // also direkt nullen und den falschen Sprung vermeiden.
                FMemory::Memzero(NewElement, ElementSize);
                FMemory::Memcpy(NewElement, Value, ElementSize);
                Raw->ArrayNum = OldNum + 1;
                return;
            }

            PS::Log<RC::LogLevel::Warning>(STR("FScriptArrayHelper::Add: Layout-Pruefung fehlgeschlagen, nutze den urspruenglichen Weg.\n"));
        }
#endif

        int32 FirstIndex = ScriptArray->Add(1, ElementSize, ElementAlignment);
        uint8* DataPtr = static_cast<uint8*>(ScriptArray->GetData());
        void* NewElementPtr = DataPtr + FirstIndex * ElementSize;
        InnerProperty->InitializeValue(NewElementPtr);
        FMemory::Memcpy(NewElementPtr, Value, ElementSize);
    }

    void FScriptArrayHelper::Add(UECustom::FManagedValue& ValuePtr)
    {
        Add(ValuePtr.GetData());
    }

    bool FScriptArrayHelper::RemoveAtIndex(RC::Unreal::int32 Index)
    {
        auto ElementSize = GetElementSize();
        auto ElementAlignment = GetMinAlignment();

        if (!ScriptArray->IsValidIndex(Index))
        {
            return false;
        }

        ScriptArray->Remove(Index, 1, ElementSize, ElementAlignment);

        return true;
    }

    void FScriptArrayHelper::Empty()
    {
        auto ElementSize = GetElementSize();
        auto ElementAlignment = GetMinAlignment();
        auto InnerProperty = GetInner();

        for (int32 Index = 0; Index < ScriptArray->Num(); ++Index)
        {
            void* ElementPtr = static_cast<uint8*>(ScriptArray->GetData()) + Index * ElementSize;
            InnerProperty->DestroyValue(ElementPtr);
        }

        ScriptArray->Empty(0, ElementSize, ElementAlignment);
    }

    void FScriptArrayHelper::InitializeValue(UECustom::FManagedValue& OutValuePtr)
    {
        auto InnerProperty = GetInner();
        PS::Trace("      INIT: GetElementSize");
        const auto Size = InnerProperty->GetElementSize();
        PS::Trace("      INIT: size=%d", (int)Size);
        void* ValuePtr = FMemory::Malloc(Size);
#ifdef __linux__
        // Siehe Add(): der virtuelle InitializeValueInternal ist unter Linux nicht
        // verifiziert und springt in die falsche Funktion.
        if (PsEngineArrayGrowEnabled()) { FMemory::Memzero(ValuePtr, Size); }
        else { InnerProperty->InitializeValue(ValuePtr); }
#else
        InnerProperty->InitializeValue(ValuePtr);
#endif
        OutValuePtr.Copy(ValuePtr);
    }

    RC::Unreal::int32 FScriptArrayHelper::GetElementSize()
    {
        auto Inner = GetInner();
        return Inner->GetElementSize();
    }

    RC::Unreal::int32 FScriptArrayHelper::GetMinAlignment()
    {
        auto Inner = GetInner();
        return Inner->GetMinAlignment();
    }

    RC::Unreal::FProperty* FScriptArrayHelper::GetInner()
    {
        return ArrayProperty->GetInner();
    }

    void FScriptArrayHelper::ForEachElement(const std::function<void(void*)> Callback)
    {
        auto ElementSize = GetElementSize();
        for (int32 Index = 0; Index < ScriptArray->Num(); ++Index)
        {
            void* ElementPtr = static_cast<uint8*>(ScriptArray->GetData()) + Index * ElementSize;
            Callback(ElementPtr);
        }
    }
}
