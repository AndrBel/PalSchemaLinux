#include "SDK/Classes/UCompositeDataTable.h"
#include "SDK/Classes/Custom/UObjectGlobals.h"
#include "Unreal/CoreUObject/UObject/Class.hpp"
#include "Helpers/Casting.hpp"
#include "SDK/Helper/PropertyHelper.h"

using namespace RC;
using namespace RC::Unreal;

namespace UECustom {
    UClass* UCompositeDataTable::StaticClass()
    {
        static auto Class = UECustom::UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.CompositeDataTable"));
        return Class;
    }

    TArray<RC::Unreal::TObjectPtr<RC::Unreal::UDataTable>>* UECustom::UCompositeDataTable::GetParentTables()
    {
        // Der bisherige feste Offset 0xb0 stammt aus dem MSVC-Build. Statt ihn fuer
        // Linux neu zu raten, wird er ueber die UE-Reflection bestimmt --
        // plattformunabhaengig und gegen Spiel-Updates robust.
        return static_cast<TArray<TObjectPtr<RC::Unreal::UDataTable>>*>(
            Palworld::PropertyHelper::GetValuePtrByPropertyNameInChain(this, STR("ParentTables")));
    }
}
