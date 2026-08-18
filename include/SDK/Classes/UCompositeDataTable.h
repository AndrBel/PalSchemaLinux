#pragma once

#include "Unreal/Engine/UDataTable.hpp"
#include "Unreal/Core/Containers/Array.hpp"

namespace UECustom {
    class UCompositeDataTable : public RC::Unreal::UDataTable {
    public:
        static RC::Unreal::UClass* StaticClass();
    public:
        // Liefert einen ZEIGER auf das ParentTables-Array im Engine-Objekt.
        // Frueher wurde das TArray per Wert zurueckgegeben: die Kopie alloziert
        // ueber UE4SS' TArray-Allokator, waehrend die Freigabe in PalSchema
        // landet. Bei zwei Tabellen faellt das nicht auf, beim Scan ueber alle
        // Composite-Tabellen zerlegte es den Heap ("mremap_chunk(): invalid
        // pointer", SIGABRT). nullptr, wenn die Property nicht auffindbar ist.
        RC::Unreal::TArray<RC::Unreal::TObjectPtr<RC::Unreal::UDataTable>>* GetParentTables();
    };
}