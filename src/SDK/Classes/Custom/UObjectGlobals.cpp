#include "Utility/LinuxCompat.h"
#include "SDK/Classes/Custom/UObjectGlobals.h"
#include "SDK/PalSignatures.h"
#include "Utility/Logging.h"

using namespace RC;
using namespace RC::Unreal;

namespace UECustom {
    UObject* UObjectGlobals::StaticFindObject(UClass* ObjectClass, UObject* InObjectPackage, const TCHAR* OrigInName, bool bExactClass)
    {
#ifdef __linux__
        // Linux: Die AOB-Signatur kann hier nicht greifen. UE4SS bringt jedoch eine
        // eigene, signaturfreie Implementierung mit (Suche ueber GUObjectArray) —
        // die ist auf dieser Plattform bereits nachweislich funktionsfaehig.
        return RC::Unreal::UObjectGlobals::StaticFindObject<UObject*>(
            ObjectClass, InObjectPackage, OrigInName, bExactClass);
#else
        using StaticFindObject_Signature = UObject*(*)(UClass*, UObject*, const TCHAR*, bool);
        static StaticFindObject_Signature StaticFindObject_Internal = nullptr;

        if (!StaticFindObject_Internal)
        {
            StaticFindObject_Internal = reinterpret_cast<StaticFindObject_Signature>(
                Palworld::SignatureManager::GetSignature("UObjectGlobals::StaticFindObject")
            );
        }

        if (!StaticFindObject_Internal)
        {
            PS::Log<LogLevel::Error>(STR("Failed to call UObjectGlobals::StaticFindObject because function address was invalid.\n"));
            return nullptr;
        }

        return StaticFindObject_Internal(ObjectClass, InObjectPackage, OrigInName, bExactClass);
#endif
    }

    void UObjectGlobals::GetObjectsOfClass(const UClass* ClassToLookFor, TArray<UObject*>& Results, bool bIncludeDerivedClasses, 
                                           EObjectFlags ExcludeFlags, EInternalObjectFlags ExclusionInternalFlags)
    {
#ifdef __linux__
        // Linux: signaturfrei ueber UE4SS' GUObjectArray-Iteration nachbilden.
        RC::Unreal::UObjectGlobals::ForEachUObject([&](UObject* Object, int32, int32) {
            if (Object)
            {
                const bool bMatches = bIncludeDerivedClasses
                    ? Object->IsA(const_cast<UClass*>(ClassToLookFor))
                    : (Object->GetClassPrivate() == ClassToLookFor);
                if (bMatches)
                {
                    Results.Add(Object);
                }
            }
            return RC::LoopAction::Continue;
        });
        return;
#else
        using GetObjectsOfClass_Signature = void(*)(const UClass*, TArray<UObject*>&, bool, EObjectFlags, EInternalObjectFlags);
        static GetObjectsOfClass_Signature GetObjectsOfClass_Internal = nullptr;

        if (!GetObjectsOfClass_Internal)
        {
            GetObjectsOfClass_Internal = reinterpret_cast<GetObjectsOfClass_Signature>(
                Palworld::SignatureManager::GetSignature("GetObjectsOfClass")
            );
        }

        if (!GetObjectsOfClass_Internal)
        {
            PS::Log<LogLevel::Error>(STR("Failed to call UObjectGlobals::GetObjectsOfClass because function address was invalid.\n"));
            return;
        }

        return GetObjectsOfClass_Internal(ClassToLookFor, Results, bIncludeDerivedClasses, ExcludeFlags, ExclusionInternalFlags);
#endif
    }
}