#include "SDK/Classes/Custom/UBlueprintGeneratedClass.h"
#include "SDK/Classes/Custom/UInheritableComponentHandler.h"
#include "SDK/Classes/Custom/UObjectGlobals.h"
#include "SDK/Helper/Memory.h"
#include "SDK/Helper/PropertyHelper.h"
#include "SDK/PalSignatures.h"
#include "Utility/Logging.h"
#include "Unreal/CoreUObject/UObject/UnrealType.hpp"

using namespace Palworld;
using namespace RC;
using namespace RC::Unreal;

namespace UECustom {
    RC::Unreal::UClass* UBlueprintGeneratedClass::StaticClass()
    {
        static auto Class = UECustom::UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, TEXT("/Script/Engine.BlueprintGeneratedClass"));
        return Class;
    }

    void UBlueprintGeneratedClass::PurgeClass(bool bRecompilingOnLoad)
    {
        using FnSignature = void(*)(UClass*, bool);
        static FnSignature fn = nullptr;

        if (!fn)
        {
            auto staticClass = StaticClass();
            // Linux: Index ist vptr-relativ (ohne Kopfeintraege) und wie alle
            // MSVC-Indizes um einen Destruktor-Slot zu klein.
            // Verifiziert an PalServer-Linux-Shipping:
            //   fnptr 120 = 0xa132e40 = PostLoadDefaultObject(UObject*)
            //     -- nimmt (this, UObject*) und ruft darauf PostLoad
            //     ("call *0xa8(%rax)"). Ein bool als UObject* zu
            //     uebergeben endet zwangslaeufig im SIGSEGV.
            //   fnptr 121 = 0xa135d30 = PurgeClass(bool) -- ruft
            //     UClass::PurgeClass (0x7a1c020) und raeumt 0x2c8 auf.
#ifdef __linux__
            constexpr size_t PurgeClassSlot = 121; // MSVC: 120
#else
            constexpr size_t PurgeClassSlot = 120;
#endif
            auto fnAddress = Palworld::GetVirtualFunctionFromClass(staticClass, PurgeClassSlot);
            fn = reinterpret_cast<FnSignature>(fnAddress);
        }

        if (!fn)
        {
            PS::Log<LogLevel::Error>(STR("Failed to call UBlueprintGeneratedClass::PurgeClass because function address was invalid.\n"));
            return;
        }

        fn(this, bRecompilingOnLoad);
    }

    UInheritableComponentHandler* UBlueprintGeneratedClass::GetInheritableComponentHandler()
    {
        auto InheritableComponentHandler = 
            PropertyHelper::GetValuePtrByPropertyNameInChain<RC::Unreal::TObjectPtr<UInheritableComponentHandler>>(this, (STR("InheritableComponentHandler")));

        if (!InheritableComponentHandler)
        {
            return nullptr;
        }

        return (*InheritableComponentHandler).Get();
    }

    USimpleConstructionScript* UBlueprintGeneratedClass::GetSimpleConstructionScript()
    {
        auto SimpleConstructionScript =
            PropertyHelper::GetValuePtrByPropertyNameInChain<RC::Unreal::TObjectPtr<USimpleConstructionScript>>(this, (STR("SimpleConstructionScript")));

        if (!SimpleConstructionScript)
        {
            return nullptr;
        }

        return (*SimpleConstructionScript).Get();
    }
}