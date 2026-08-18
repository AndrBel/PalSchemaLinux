#include "Unreal/UObjectGlobals.hpp"
#include "Utility/LinuxFormat.h"
#include <regex>
#include "Unreal/CoreUObject/UObject/Class.hpp"
#include "Unreal/CoreUObject/UObject/UnrealType.hpp"
#include "Unreal/Property/FEnumProperty.hpp"
#include "Unreal/UObject.hpp"
#include "Unreal/AActor.hpp"
#include "Helpers/String.hpp"
#include "SDK/Helper/PropertyHelper.h"
#include "Utility/Config.h"
#include "Utility/Logging.h"
#include "Utility/JsonHelpers.h"
#include "Loader/PalBlueprintModLoader.h"
#include "SDK/Classes/KismetSystemLibrary.h"
#include "SDK/Helper/BPGeneratedClassHelper.h"
#include "SDK/Helper/Memory.h"
#include "SDK/Classes/Custom/UBlueprintGeneratedClass.h"
#include "SDK/Classes/Custom/UInheritableComponentHandler.h"
#include "SDK/Classes/Custom/UObjectGlobals.h"
#include "Unreal/Engine/UDataTable.hpp"
#include "Utility/PsTrace.h"
#include "SDK/Structs/Custom/FScriptArrayHelper.h"
#include <unordered_map>

using namespace RC;
using namespace RC::Unreal;

namespace {
    // Messpunkt (2026-08-16): TargetTypesA wird von keinem Mod geschrieben -- den
    // bestehenden Ist-Zustand rein lesend protokollieren (kein Add/Empty, nur
    // ForEachElement), um zu pruefen, ob DogCoin.TypeA (Material) dort schon drinsteht.
    void PsTraceComponentArray(RC::Unreal::UObject* component, const RC::StringType& propertyName, const char* label)
    {
        if (!component)
        {
            PS::Trace("%s: kein Component", label);
            return;
        }

        // Bewusst die nicht-templated Ueberladung + CastField (nicht GetPropertyByName<T>/
        // Palworld::PropertyHelper::CastProperty<T>): Der templatisierte GetPropertyByName<T>
        // ruft CastProperty<T> auf, bevor dessen Definition im selben Header sichtbar ist --
        // GCCs Two-Phase-Lookup bindet das an eine nie definierte globale Vorwaerts-
        // deklaration, was erst beim Linken als "undefined symbol" auffaellt (mit
        // FArrayProperty getestet, 2026-08-16). CastField ist davon nicht betroffen.
        auto rawProperty = Palworld::PropertyHelper::GetPropertyByName(component->GetClassPrivate(), propertyName);
        auto property = CastField<FArrayProperty>(rawProperty);
        if (!property)
        {
            PS::Trace("%s: Property nicht gefunden", label);
            return;
        }

        auto dataPtr = property->ContainerPtrToValuePtr<void>(component);
        auto scriptArray = static_cast<FScriptArray*>(dataPtr);
        UECustom::FScriptArrayHelper helper(scriptArray, property);
        auto inner = helper.GetInner();

        int index = 0;
        helper.ForEachElement([&](void* elemData) {
            char itemLabel[64];
            std::snprintf(itemLabel, sizeof(itemLabel), "%s[%d]", label, index);
            Palworld::PropertyHelper::TraceScalarValue(itemLabel, inner, elemData);
            ++index;
        });
        PS::Trace("%s: Anzahl=%d", label, index);
    }

    // Messpunkt (2026-08-18): Hilfsfunktionen fuer den RecipeIds-Cache-Fix. Liest eine
    // Enum-/Zahlen-Property roh als int64 -- exakt dasselbe Verfahren wie
    // PropertyHelper::TraceScalarValue (siehe dort), hier aber zum Vergleichen statt Loggen.
    bool PsReadRawIntegral(RC::Unreal::FProperty* Property, void* Data, int64_t& Out)
    {
        if (!Property || !Data) return false;
        auto Size = Property->GetElementSize();
        switch (Size)
        {
        case 1: Out = *static_cast<uint8_t*>(Data);  return true;
        case 2: Out = *static_cast<uint16_t*>(Data); return true;
        case 4: Out = *static_cast<uint32_t*>(Data); return true;
        case 8: Out = *static_cast<int64_t*>(Data);  return true;
        default: return false;
        }
    }

    // Prueft, ob eine Enum-/Zahlen-ArrayProperty (z.B. TargetTypesA/TargetTypesB) einen
    // bestimmten Rohwert enthaelt. Nutzt dieselbe CastField-Regel wie PsTraceComponentArray
    // (nicht GetPropertyByName<T>/CastProperty<T>, siehe dortiger Kommentar).
    bool PsArrayContainsRawValue(RC::Unreal::UObject* obj, const RC::StringType& propertyName, int64_t wanted)
    {
        if (!obj) return false;
        auto rawProperty = Palworld::PropertyHelper::GetPropertyByName(obj->GetClassPrivate(), propertyName);
        auto property = CastField<FArrayProperty>(rawProperty);
        if (!property) return false;

        auto dataPtr = property->ContainerPtrToValuePtr<void>(obj);
        auto scriptArray = static_cast<FScriptArray*>(dataPtr);
        UECustom::FScriptArrayHelper helper(scriptArray, property);
        auto inner = helper.GetInner();

        bool found = false;
        helper.ForEachElement([&](void* elemData) {
            if (found) return;
            int64_t value = 0;
            if (PsReadRawIntegral(inner, elemData, value) && value == wanted) found = true;
        });
        return found;
    }

    // Prueft, ob RecipeIds (FNameProperty-Array) einen bestimmten Namen bereits enthaelt --
    // verhindert doppelte Eintraege, falls der Fix mehrfach laeuft (z.B. nach einem erneuten
    // Neustart mit bereits gepatchten Instanzen, sollte das je vorkommen).
    bool PsRecipeIdsContains(RC::Unreal::UObject* model, const RC::Unreal::FName& name)
    {
        if (!model) return false;
        auto rawProperty = Palworld::PropertyHelper::GetPropertyByName(model->GetClassPrivate(), STR("RecipeIds"));
        auto property = CastField<FArrayProperty>(rawProperty);
        if (!property) return false;
        auto* nameInner = CastField<FNameProperty>(property->GetInner());
        if (!nameInner) return false;

        auto dataPtr = property->ContainerPtrToValuePtr<void>(model);
        auto scriptArray = static_cast<FScriptArray*>(dataPtr);
        UECustom::FScriptArrayHelper helper(scriptArray, property);

        bool found = false;
        helper.ForEachElement([&](void* elemData) {
            if (found) return;
            if (nameInner->GetPropertyValue(elemData) == name) found = true;
        });
        return found;
    }

    // Messpunkt (2026-08-16): alle drei bekannten Kandidaten (Patch landet nicht, TypeB-
    // Mismatch, TypeA-Mismatch) sind widerlegt. Naechster Verdacht: ein von diesem Mod nie
    // gesetztes Feld in der Rezeptzeile selbst (z.B. eine Technologie-Freischaltung) haelt
    // das Rezept unsichtbar. Alle Felder der Zeile auflisten, statt eine Vermutung zu raten
    // -- TraceScalarValue liest bei Nicht-Enum/-Zahl nur die Klasse, fasst Data nicht an,
    // ist also auch fuer verschachtelte Structs/Strings gefahrlos.
    void PsTraceRowFields(RC::Unreal::UScriptStruct* rowStruct, void* rowData, const char* label)
    {
        if (!rowStruct || !rowData)
        {
            PS::Trace("%s: rowStruct oder rowData ist null", label);
            return;
        }

        auto field = rowStruct->GetChildProperties();
        while (field)
        {
            auto* prop = static_cast<FProperty*>(field);
            auto propName = RC::to_string(prop->GetName());
            char itemLabel[96];
            std::snprintf(itemLabel, sizeof(itemLabel), "%s.%s", label, propName.c_str());
            auto valuePtr = prop->ContainerPtrToValuePtr<void>(rowData);
            Palworld::PropertyHelper::TraceScalarValue(itemLabel, prop, valuePtr);
            field = Palworld::PropertyHelper::GetNextField(field);
        }
    }
}

namespace {
// ---------------------------------------------------------------------------
// Linux-Portierung: vtable-Slots von PostLoad / PostInitializeComponents
//
// PalSchema adressiert diese beiden Virtuals ueber fest verdrahtete
// vtable-Indizes aus dem MSVC-Build (20 bzw. 159). Unter dem Itanium-C++-ABI
// (Linux/Clang) hat jede polymorphe Klasse ZWEI Destruktor-Slots (D1 = complete
// object, D0 = deleting), MSVC nur einen. Alle Indizes sind unter Linux daher
// um genau einen Slot groesser -- dieselbe Verschiebung, die PropertyHelper.cpp
// bereits als "MSVC-Offset + 8" korrigiert.
//
// Verifiziert an PalServer-Linux-Shipping (EXEC/non-PIE, Adressen stabil):
//
//   UObject-vtable, Funktionszeiger-Index 20 = 0x4410e30 = "mov $1,%al; ret".
//     Dieser Stub wird in FAsyncPackage::PostLoadObjects (0x79e0ac0) bei
//     0x79e0d98 aufgerufen:
//         mov (%r12),%rax ; call *0xa0(%rax) ; test %al,%al ; je <abbruch>
//     0xa0/8 = 20 => das ist UObject::IsReadyForAsyncPostLoad(). Liefert es
//     false, verschiebt der Loader das Objekt und versucht es erneut -- endlos.
//     Der bisherige Hook legte einen Detour mit void-Rueckgabe auf diese
//     bool-Funktion: AL enthielt danach Muell. Wurde AL zu 0, kehrte
//     FlushAsyncLoading nie zurueck => "Hang detected on GameThread".
//     Sichtbar wurde das erst beim Spielerbeitritt, weil ein Dedicated Server
//     ohne Spieler keine World-Partition-Zellen asynchron nachlaedt.
//     Zusaetzlich ist 0x4410e30 die von SAEMTLICHEN UObject-Klassen geteilte
//     Default-Implementierung -- der Hook traf damit die ganze Engine.
//
//   Index 21 = 0x7af63c0 wird von UObject::ConditionalPostLoad (0x7af8120)
//     per "jmp *0xa8(%rax)" angesprungen (0xa8/8 = 21) => das ist PostLoad.
//     UBlueprintGeneratedClass ueberschreibt genau diesen Slot (0xa1323a0).
//
//   AActor-vtable Index 159 = 0x9f860f0 = PreInitializeComponents (liest
//     AutoReceiveInput bei 0x153, ruft GetPlayerController/EnableInput),
//     Index 160 = 0x9f860e0 = PostInitializeComponents (Validitaetscheck,
//     setzt bActorInitialized bei 0x5c, ruft UpdateAllReplicatedComponents).
//     APalPlayerState ueberschreibt Index 160, nicht 159.
// ---------------------------------------------------------------------------
#ifdef __linux__
    constexpr size_t PS_VT_SLOT_POSTLOAD = 21;              // MSVC: 20
    constexpr size_t PS_VT_SLOT_POSTINITCOMPONENTS = 160;   // MSVC: 159
#else
    constexpr size_t PS_VT_SLOT_POSTLOAD = 20;
    constexpr size_t PS_VT_SLOT_POSTINITCOMPONENTS = 159;
#endif

    // Schutznetz gegen einen erneuten Griff in den falschen Slot: die
    // Default-Implementierungen vieler UObject-Virtuals sind winzige, von allen
    // Klassen GETEILTE Stubs ("ret", "xor eax,eax; ret", "mov $imm8,%al; ret").
    // Ein Inline-Hook darauf ueberschreibt eine Funktion, die die halbe Engine
    // benutzt, und faelscht bei bool-Virtuals zusaetzlich den Rueckgabewert.
    // Wer hier landet, hat den Slot verfehlt => lieber gar nicht hooken.
    bool PsIsTrivialStub(const void* fn)
    {
        if (!fn) return true;
        const auto* b = static_cast<const unsigned char*>(fn);
        if (b[0] == 0xC3) return true;                                   // ret
        if (b[0] == 0x31 && b[1] == 0xC0 && b[2] == 0xC3) return true;   // xor eax,eax; ret
        if (b[0] == 0x33 && b[1] == 0xC0 && b[2] == 0xC3) return true;   // xor eax,eax (alt.); ret
        if (b[0] == 0xB0 && b[2] == 0xC3) return true;                   // mov $imm8,%al; ret
        return false;
    }
}

namespace Palworld {
    PalBlueprintModLoader::PalBlueprintModLoader() : PalModLoaderBase("blueprints")
    {
        SetDisplayName(TEXT("Blueprint Mod Loader"));
    }

    PalBlueprintModLoader::~PalBlueprintModLoader()
    {
        auto expected = PostLoadHook.disable();
        PostLoadHook = {};
        PostLoadCallback = nullptr;
        m_modsMap.clear();
    }

    void PalBlueprintModLoader::OnLoad(const std::filesystem::path& loaderPath, const RC::StringType& modName, const EEngineLifecyclePhase& engineLifecyclePhase)
    {
        if (engineLifecyclePhase == EEngineLifecyclePhase::PostEngineInit)
        {
            PS::JsonHelpers::ParseJsonFilesInPath(loaderPath, [&](const nlohmann::json& data) {
                LoadSafe(data);
            });
        }
        else if (engineLifecyclePhase == EEngineLifecyclePhase::GameInstanceInit)
        {
            PS::JsonHelpers::ParseJsonFilesInPath(loaderPath, [&](const nlohmann::json& data) {
                LoadUnsafe(data);
            });
        }
    }

    void PalBlueprintModLoader::OnAutoReload(const RC::StringType& modName, const std::filesystem::path& modFilePath)
    {
        PS::JsonHelpers::ParseJsonFileInPath(modFilePath, [&](const nlohmann::json& data) {
            LoadUnsafe(data);
        });
    }

    bool PalBlueprintModLoader::CanInitialize(const EEngineLifecyclePhase& engineLifecyclePhase)
    {
        if (engineLifecyclePhase == EEngineLifecyclePhase::PostEngineInit)
        {
            return true;
        }

        return false;
    }

    bool PalBlueprintModLoader::OnInitialize()
    {
        // PostLoad hook handles default objects for classes
        if (!HookPostLoad())
        {
            PS::Log<LogLevel::Error>(TEXT("Cannot hook UBlueprintGeneratedClass::PostLoad which means blueprint mods will not function properly.\n"));
            return false;
        }

        // PostInitComponents hook handles actor instances
        if (!HookPostInitComponents())
        {
            PS::Log<LogLevel::Error>(TEXT("Cannot hook AActor::PostInitComponents which means blueprint mods will not function properly.\n"));
            return false;
        }

        return true;
    }

    bool PalBlueprintModLoader::HookPostLoad()
    {
        auto vtable = Palworld::GetVTablePtrByClassPath(TEXT("/Script/Engine.BlueprintGeneratedClass"));
        if (!vtable)
        {
            PS::Log<LogLevel::Error>(STR("Something went wrong with getting VTable pointer for UBlueprintGeneratedClass."));
            return false;
        }

        void* postloadPtr = Palworld::GetVirtualFunctionFromVTable(vtable, PS_VT_SLOT_POSTLOAD);
        if (PsIsTrivialStub(postloadPtr))
        {
            PS::Log<LogLevel::Error>(TEXT("Refusing to hook UBlueprintGeneratedClass::PostLoad at {}: target is a shared trivial UObject stub, so the vtable slot is wrong. Leaving blueprint mods disabled instead of breaking async loading.\n"), postloadPtr);
            return false;
        }
        PS::Log<LogLevel::Normal>(TEXT("Found UBlueprintGeneratedClass::PostLoad: {} (vtable slot {}).\n"), postloadPtr, static_cast<int>(PS_VT_SLOT_POSTLOAD));

        PostLoadCallback = [&](UClass* actorClass) {
            ModifyObject(actorClass->GetClassDefaultObject());
        };

        PostLoadHook = safetyhook::create_inline(postloadPtr,
            reinterpret_cast<void*>(PostLoad));

        return true;
    }

    bool PalBlueprintModLoader::HookPostInitComponents()
    {
        auto vtable = Palworld::GetVTablePtrByClassPath(TEXT("/Script/Engine.Actor"));
        if (!vtable)
        {
            PS::Log<LogLevel::Error>(TEXT("Something went wrong with getting VTable pointer for AActor.\n"));
            return false;
        }

        void* postInitCompsPtr = Palworld::GetVirtualFunctionFromVTable(vtable, PS_VT_SLOT_POSTINITCOMPONENTS);
        if (PsIsTrivialStub(postInitCompsPtr))
        {
            PS::Log<LogLevel::Error>(TEXT("Refusing to hook AActor::PostInitializeComponents at {}: target is a shared trivial UObject stub, so the vtable slot is wrong. Leaving blueprint mods disabled instead of breaking actor initialization.\n"), postInitCompsPtr);
            return false;
        }
        PS::Log<LogLevel::Normal>(TEXT("Found AActor::PostInitializeComponents: {} (vtable slot {}).\n"), postInitCompsPtr, static_cast<int>(PS_VT_SLOT_POSTINITCOMPONENTS));

        PostInitComponentsCallback = [&](AActor* self) {
            ModifyObject(self);
        };

        PostInitComponentsHook = safetyhook::create_inline(postInitCompsPtr,
            reinterpret_cast<void*>(PostInitComponents));

        return true;
    }

    void PalBlueprintModLoader::LoadSafe(const nlohmann::json& data)
    {
        for (auto& [assetName, assetData] : data.items())
        {
            auto assetKey = assetName;
#ifdef __linux__
            // Upstream landen /Game/-Pfade in LoadUnsafe -- und damit in der Phase
            // GameInstanceInit, die unter Linux abgeschaltet ist, ueber ein
            // LoadAsset_Blocking, das die Pfade ohnehin nicht aufloest. Zwei Blocker
            // uebereinander, das Ergebnis ist ein wirkungsloser Mod.
            // Stattdessen den Klassennamen aus dem Pfad ableiten und wie einen
            // gewoehnlichen Eintrag registrieren. Angewandt wird er spaeter in
            // ApplyToExistingObjects() ueber die bereits geladenen Klassenobjekte.
            //   /Game/.../BP_X.BP_X_C -> BP_X_C
            //   /Game/.../BP_X        -> BP_X_C
            if (assetKey.starts_with("/Game/"))
            {
                auto dotPos = assetKey.find_last_of('.');
                if (dotPos != std::string::npos)
                {
                    assetKey = assetKey.substr(dotPos + 1);
                }
                else
                {
                    auto slashPos = assetKey.find_last_of('/');
                    assetKey = (slashPos == std::string::npos ? assetKey : assetKey.substr(slashPos + 1)) + "_C";
                }
            }
#endif
            auto assetNameWide = RC::to_generic_string(assetKey);
            if (!assetNameWide.starts_with(TEXT("/Game/")))
            {
                auto assetFName = FName(assetNameWide, FNAME_Add);
                auto newMod = PalBlueprintMod(assetFName, assetData);
                auto it = m_modsMap.find(assetFName);
                if (it != m_modsMap.end())
                {
                    m_modsMap.at(assetFName).push_back(newMod);
                }
                else
                {
                    auto newModContainer = std::vector<PalBlueprintMod>{
                        newMod
                    };
                    m_modsMap.emplace(assetFName, newModContainer);
                }

                PS::Log<LogLevel::Normal>(STR("Loaded changes to {}\n"), assetNameWide);
            }
        }
    }

    void PalBlueprintModLoader::LoadUnsafe(const nlohmann::json& data)
    {
        for (auto& [assetName, assetData] : data.items())
        {
            auto assetNameWide = RC::to_generic_string(assetName);
            if (assetNameWide.starts_with(TEXT("/Game/")))
            {
                // Linux-Portierung: std::regex unterstuetzt kein char16_t.
                // Daher ueber UTF-8 gehen und das Ergebnis zurueckwandeln.
                static const std::regex Pattern(R"(^(.*/)([^/.]+)$)");
                auto assetNameNarrow = RC::to_string(assetNameWide);
                assetNameNarrow = std::regex_replace(assetNameNarrow, Pattern, "$1$2.$2_C");
                assetNameWide = RC::to_wstring(assetNameNarrow);

                auto softObjectPtr = UECustom::TSoftObjectPtr<UObject>(UECustom::FSoftObjectPath(assetNameWide));
                auto asset = UECustom::UKismetSystemLibrary::LoadAsset_Blocking(softObjectPtr);
                if (!asset)
                {
                    throw std::runtime_error(RC::fmt("Failed to apply blueprint changes, asset '%S' was invalid", assetNameWide.c_str()));
                }

                asset->SetRootSet();

                auto& defaultObject = static_cast<UClass*>(asset)->GetClassDefaultObject();
                ApplyData(assetData, defaultObject.Get());

                PS::Log<RC::LogLevel::Normal>(TEXT("Applied changes to {}\n"), static_cast<UClass*>(asset)->GetNamePrivate().ToString());
            }
        }
    }

    std::vector<PalBlueprintMod>& PalBlueprintModLoader::GetModsForBlueprint(const RC::Unreal::FName& name)
    {
        auto it = m_modsMap.find(name);
        if (it != m_modsMap.end())
        {
            return it->second;
        }

        throw std::runtime_error(RC::fmt("Failed to get mods for this blueprint. Affected mod name: %S", name.ToString().c_str()));
    }

    void PalBlueprintModLoader::ModifyObject(RC::Unreal::UObject* object)
    {
        if (!object) return;

        auto objectClass = object->GetClassPrivate();
        auto& objectName = objectClass->GetNamePrivate();

        if (!m_modsMap.contains(objectName))
        {
            return;
        }

        auto& mods = GetModsForBlueprint(objectName);
        for (auto& mod : mods)
        {
            try
            {
                ApplyMod(mod, object);
            }
            catch (const std::exception& e)
            {
                PS::Log<RC::LogLevel::Error>(TEXT("Failed modifying blueprint '{}', {}\n"), objectName.ToString(), RC::to_generic_string(e.what()));
            }
        }
    }

    void PalBlueprintModLoader::ApplyMod(const PalBlueprintMod& mod, UObject* object)
    {
        auto& data = mod.GetData();
        ApplyData(data, object);
    }

    void PalBlueprintModLoader::ApplyData(const nlohmann::json& data, RC::Unreal::UObject* object)
    {
        auto objectClass = static_cast<UECustom::UBlueprintGeneratedClass*>(object->GetClassPrivate());

        for (auto& [propertyName, propertyValue] : data.items())
        {
            auto propertyNameWide = RC::to_generic_string(propertyName);
            auto property = Palworld::PropertyHelper::GetPropertyByName(objectClass, propertyNameWide);
            
            if (!property)
            {
                PS::Log<RC::LogLevel::Warning>(TEXT("Property '{}' does not exist in {}\n"), propertyNameWide, objectClass->GetNamePrivate().ToString());
                continue;
            }

            if (auto objectProperty = CastField<FObjectProperty>(property))
            {
                auto objectValue = *property->ContainerPtrToValuePtr<UObject*>(object);
                if (!objectValue)
                {
                    // null Object means that this property could be a component template, so we should check if it has an associated GEN_VARIABLE.
                    HandleInheritableComponent(objectClass, propertyNameWide, propertyValue);
                }
                else
                {
                    // Object has a pointer assigned to it so we let PropertyHelper handle it.
                    PropertyHelper::CopyJsonValueToContainer(object, property, propertyValue);
                }
            }
            else
            {
                // Any other property values get handled here like Numeric, Bool, String, etc.
                PropertyHelper::CopyJsonValueToContainer(object, property, propertyValue);
            }
        }
    }

    void PalBlueprintModLoader::HandleInheritableComponent(UECustom::UBlueprintGeneratedClass* bpClass, const RC::StringType& componentName,
                                                         const nlohmann::json& componentData)
    {
        auto& bpClassName = bpClass->GetNamePrivate();

        if (!componentData.is_object())
        {
            PS::Log<LogLevel::Warning>(TEXT("{} failed to apply, provided JSON value wasn't an object\n"), bpClassName.ToString());
            return;
        }

        auto componentFullName = PS::Format("{}_GEN_VARIABLE", componentName);
        UObject* inheritableComponent = nullptr;

        auto inheritableComponentHandler = bpClass->GetInheritableComponentHandler();
        if (inheritableComponentHandler)
        {
            auto records = inheritableComponentHandler->GetRecords();
            int32_t recordCount = 0;
            for (auto& record : records)
            {
                ++recordCount;
                if (record.ComponentTemplate.Get() == nullptr) continue;

                if (record.ComponentTemplate.Get()->GetName() == componentFullName)
                {
                    inheritableComponent = record.ComponentTemplate.Get();
                    break;
                }
            }
            PS::Trace("  HandleInheritableComponent: gesucht='%s' ICH-Records=%d gefunden=%d",
                RC::to_string(componentFullName).c_str(), recordCount, inheritableComponent != nullptr);
        }
        else
        {
            PS::Trace("  HandleInheritableComponent: gesucht='%s' kein InheritableComponentHandler",
                RC::to_string(componentFullName).c_str());
        }

        if (inheritableComponent)
        {
            ModifyComponent(inheritableComponent, componentData);
            return;
        }

        // Component wasn't inside Inheritable Components list, so check SimpleConstructionScript next.
        if (!HandleNodeComponent(bpClass, componentFullName, componentData))
        {
            // Messpunkt Werkbank-Rezept (2026-08-15): bisher schlug dieser Fall lautlos fehl --
            // ApplyMod/ModifyObject meldeten trotzdem "erfolgreich gepatcht", weil sie nur
            // pruefen, ob ApplyData ohne Exception durchlief, nicht ob eine Component-Property
            // ueberhaupt geschrieben wurde. Kandidat 1 ("Der Patch landet nicht") direkt sichtbar
            // machen statt ihn ueber eine Erfolgsmeldung zu verdecken.
            PS::Log<LogLevel::Warning>(TEXT("Component-Template fuer '{}' weder im InheritableComponentHandler noch in der SimpleConstructionScript von {} gefunden -- Aenderung wirkungslos.\n"),
                componentFullName, bpClassName.ToString());
        }
    }

    bool PalBlueprintModLoader::HandleNodeComponent(UECustom::UBlueprintGeneratedClass* bpClass, const RC::StringType& componentName, const nlohmann::json& componentData)
    {
        auto simpleConstructionScript = bpClass->GetSimpleConstructionScript();
        if (!simpleConstructionScript)
        {
            PS::Trace("  HandleNodeComponent: gesucht='%s' keine SimpleConstructionScript", RC::to_string(componentName).c_str());
            return false;
        }

        UObject* nodeComponent = nullptr;

        auto& nodes = simpleConstructionScript->GetAllNodes();
        int32_t nodeCount = 0;
        for (auto& nodeElement : nodes)
        {
            ++nodeCount;
            auto nodeComponentTemplate = nodeElement->GetComponentTemplate();
            if (!nodeComponentTemplate)
            {
                continue;
            }

            if (nodeComponentTemplate->GetName() == componentName)
            {
                nodeComponent = nodeComponentTemplate;
                break;
            }
        }
        PS::Trace("  HandleNodeComponent: gesucht='%s' SCS-Nodes=%d gefunden=%d",
            RC::to_string(componentName).c_str(), nodeCount, nodeComponent != nullptr);

        if (!nodeComponent)
        {
            return false;
        }

        ModifyComponent(nodeComponent, componentData);
        return true;
    }

    void PalBlueprintModLoader::ModifyComponent(RC::Unreal::UObject* component, const nlohmann::json& componentData)
    {
#ifdef __linux__
        // Messpunkt Werkbank-Rezept, dritte Spur (2026-08-16): TargetTypesA wird von diesem
        // Mod nie geschrieben -- pruefen, ob DogCoin.TypeA (Material) im bestehenden
        // Ist-Zustand ueberhaupt enthalten ist.
        PsTraceComponentArray(component, STR("TargetTypesA"), "TargetTypesA-IST");

        // Messpunkt (2026-08-16), vierte Spur: TargetRankMax der Station gegen Rank des
        // Items (siehe DogCoin-Zeilendump in ApplyToExistingObjects). Skalar, kein Array --
        // dieselbe CastField-Regel wie bei PsTraceComponentArray (nicht CastProperty<T>).
        if (auto* rankProp = CastField<FNumericProperty>(Palworld::PropertyHelper::GetPropertyByName(component->GetClassPrivate(), STR("TargetRankMax"))))
        {
            PropertyHelper::TraceScalarValue("TargetRankMax-IST", rankProp, rankProp->ContainerPtrToValuePtr<void>(component));
        }
        else
        {
            PS::Trace("TargetRankMax-IST: Property nicht gefunden");
        }
#endif
        for (auto& [innerKey, innerValue] : componentData.items())
        {
            auto componentPropertyName = RC::to_generic_string(innerKey);
            auto componentProperty = PropertyHelper::GetPropertyByName(component->GetClassPrivate(), componentPropertyName.c_str());
            if (!componentProperty)
            {
                PS::Log<LogLevel::Warning>(TEXT("Property {} doesn't exist in {}\n"), componentPropertyName, component->GetName());
                continue;
            }

            PropertyHelper::CopyJsonValueToContainer(component, componentProperty, innerValue);
        }
    }

#ifdef __linux__
    void PalBlueprintModLoader::ApplyToExistingObjects()
    {
        if (m_modsMap.empty()) return;

        // Dasselbe tun, was der PostLoad-Hook getan haette: das Default Object jeder
        // passenden BlueprintGeneratedClass patchen. Klassenobjekt ueber den Namen
        // seiner eigenen Klasse erkennen -- FName-Vergleich, also ein Integer-Vergleich
        // ohne Allokation (siehe PalMainLoader::ScanExistingDataTables).
        static const FName BlueprintGeneratedClassFName(STR("BlueprintGeneratedClass"));
        int32_t applied = 0;

        UObjectGlobals::ForEachUObject([&](UObject* Object, ::RC::Unreal::int32, ::RC::Unreal::int32) {
            if (!Object) return RC::LoopAction::Continue;
            auto* Cls = Object->GetClassPrivate();
            if (!Cls) return RC::LoopAction::Continue;
            if (Cls->GetNamePrivate() != BlueprintGeneratedClassFName) return RC::LoopAction::Continue;
            if (!m_modsMap.contains(Object->GetNamePrivate())) return RC::LoopAction::Continue;

            auto& defaultObject = static_cast<UClass*>(Object)->GetClassDefaultObject();
            if (!defaultObject.Get()) return RC::LoopAction::Continue;

            ModifyObject(defaultObject.Get());
            ++applied;
            PS::Log<LogLevel::Normal>(STR("  nachtraeglich gepatcht: {}\n"), Object->GetNamePrivate().ToString());
            return RC::LoopAction::Continue;
        });

        PS::Log<LogLevel::Normal>(STR("Blueprint-Loader: {} bereits geladene Klassen nachtraeglich gepatcht.\n"), applied);

        // Messpunkt Werkbank-Rezept (2026-08-15): Kandidat 2 ("DogCoin ist nicht vom Typ
        // MaterialJewelry") direkt gegenpruefen. ScanExistingDataTables() ist zu diesem
        // Zeitpunkt bereits gelaufen (PalMainLoader::Initialize() ruft es vor der Schleife
        // ueber ApplyToExistingObjects() auf), DT_ItemDataTable ist also registriert.
        if (auto* itemTable = TryGetDatatableByName("DT_ItemDataTable"))
        {
            static const FName DogCoinRowName(STR("DogCoin"));
            if (auto* row = itemTable->FindRowUnchecked(DogCoinRowName))
            {
                auto rowStruct = itemTable->GetRowStruct().Get();
                if (rowStruct)
                {
                    // Messpunkt (2026-08-16): gezielte Felder statt PsTraceRowFields (voller
                    // Struct-Walk via GetNextField) -- letzterer haengt/stuerzt auf dieser
                    // Tabelle (beobachtet nach DogCoin.FloatValue1, kein Absturzsignal, aber
                    // keine weitere Zeile mehr). Alle Feldwerte liegen bereits aus einem
                    // frueheren, sauberen Durchlauf vor (Rank=4, TypeA=Material,
                    // TypeB=MaterialJewelry, bEnableHandcraft war false). Hier nur gezielt
                    // gegenpruefen, dass bEnableHandcraft jetzt true ist.
                    if (auto* handcraftProp = CastField<FBoolProperty>(Palworld::PropertyHelper::GetPropertyByName(rowStruct, STR("bEnableHandcraft"))))
                    {
                        PropertyHelper::TraceScalarValue("DogCoin.bEnableHandcraft", handcraftProp, handcraftProp->ContainerPtrToValuePtr<void>(row));
                    }
                }
                else
                {
                    PS::Trace("DogCoin: DT_ItemDataTable hat keinen RowStruct");
                }
            }
            else
            {
                PS::Trace("DogCoin: Row nicht in DT_ItemDataTable gefunden");
            }
        }
        else
        {
            PS::Trace("DT_ItemDataTable: nicht gefunden (TryGetDatatableByName)");
        }

        // Messpunkt (2026-08-16b): PsTraceRowFields (voller Struct-Walk ueber
        // GetNextField, hardcodierter 0x20-Feldketten-Offset, nie unabhaengig auf Linux
        // verifiziert) haengt jetzt AUCH auf DT_ItemRecipeDataTable -- beobachtet in einem
        // Lauf, in dem die letzte Trace-Zeile "DogCoin.bEnableHandcraft" war und danach
        // ueber eine Stunde keine weitere Zeile mehr kam, obwohl Prozess/REST-API am Leben
        // blieben (kein Absturzsignal). Widerspricht einer frueheren Notiz, dieselbe Tabelle
        // sei einmal sauber durchgelaufen -- also layout-/lauf-abhaengig und nicht sicher
        // wiederholbar. Deshalb hier bewusst NUR das eine gesuchte Feld lesen:
        // UnlockItemID (FNameProperty), gezielt per GetPropertyByName + CastField, kein
        // Struct-Walk. Zusaetzlich als Referenz dieselbe Property bei einem beliebigen
        // regulaer herstellbaren Item (bEnableHandcraft=true in DT_ItemDataTable) auslesen,
        // um zu sehen, ob UnlockItemID bei craftbaren Items ueblicherweise gesetzt ist.
        if (auto* recipeTable = TryGetDatatableByName("DT_ItemRecipeDataTable"))
        {
            auto recipeRowStruct = recipeTable->GetRowStruct().Get();
            auto* unlockProp = recipeRowStruct
                ? CastField<FNameProperty>(Palworld::PropertyHelper::GetPropertyByName(recipeRowStruct, STR("UnlockItemID")))
                : nullptr;

            if (!unlockProp)
            {
                PS::Trace("DogCoinRecipe: UnlockItemID-Property nicht gefunden (rowStruct=%p)", (void*)recipeRowStruct);
            }
            else
            {
                static const FName DogCoinRecipeRowName(STR("DogCoin"));
                if (auto* recipeRow = recipeTable->FindRowUnchecked(DogCoinRecipeRowName))
                {
                    PropertyHelper::TraceScalarValue("DogCoin.UnlockItemID", unlockProp, unlockProp->ContainerPtrToValuePtr<void>(recipeRow));

                    // Messpunkt (2026-08-16d): Product_Id wird vom Mod EXPLIZIT gesetzt (im
                    // Gegensatz zu UnlockItemID). Vergleich: ist die Korruption spezifisch fuer
                    // ein von AddRow nie beruehrtes Feld, oder betrifft sie JEDE FNameProperty
                    // auf einer per AddRow neu angelegten Zeile -- auch vom Mod korrekt
                    // beschriebene?
                    if (auto* productIdProp = CastField<FNameProperty>(Palworld::PropertyHelper::GetPropertyByName(recipeRowStruct, STR("Product_Id"))))
                    {
                        PropertyHelper::TraceScalarValue("DogCoin.Product_Id", productIdProp, productIdProp->ContainerPtrToValuePtr<void>(recipeRow));
                    }
                    if (auto* material1IdProp = CastField<FNameProperty>(Palworld::PropertyHelper::GetPropertyByName(recipeRowStruct, STR("Material1_Id"))))
                    {
                        PropertyHelper::TraceScalarValue("DogCoin.Material1_Id", material1IdProp, material1IdProp->ContainerPtrToValuePtr<void>(recipeRow));
                    }

                    // Messpunkt (2026-08-18b): Balance-Anpassung -- WorkAmount einer normalen,
                    // handwerklich hergestellten Referenz (PalSphere) auslesen, um die
                    // Mog-Muenze daran auszurichten statt einen Wert zu raten. Einzelnes
                    // Skalarfeld per GetPropertyByName + CastField, kein Struct-Walk (sicher).
                    if (auto* palSphereRow = recipeTable->FindRowUnchecked(FName(STR("PalSphere"))))
                    {
                        if (auto* workAmountProp = CastField<FNumericProperty>(Palworld::PropertyHelper::GetPropertyByName(recipeRowStruct, STR("WorkAmount"))))
                        {
                            PropertyHelper::TraceScalarValue("PalSphere.WorkAmount", workAmountProp, workAmountProp->ContainerPtrToValuePtr<void>(palSphereRow));
                        }
                        if (auto* productCountProp = CastField<FNumericProperty>(Palworld::PropertyHelper::GetPropertyByName(recipeRowStruct, STR("Product_Count"))))
                        {
                            PropertyHelper::TraceScalarValue("PalSphere.Product_Count", productCountProp, productCountProp->ContainerPtrToValuePtr<void>(palSphereRow));
                        }
                        if (auto* mat1CountProp = CastField<FNumericProperty>(Palworld::PropertyHelper::GetPropertyByName(recipeRowStruct, STR("Material1_Count"))))
                        {
                            PropertyHelper::TraceScalarValue("PalSphere.Material1_Count", mat1CountProp, mat1CountProp->ContainerPtrToValuePtr<void>(palSphereRow));
                        }
                    }
                    else
                    {
                        PS::Trace("PalSphere: Row nicht in DT_ItemRecipeDataTable gefunden");
                    }

                    // Messpunkt (2026-08-17), zurueckgenommen: ein WorkableAttribute-Leseversuch
                    // hier (identisches Muster wie die drei erfolgreichen Aufrufe direkt darueber
                    // -- GetPropertyByName(recipeRowStruct, ...) + CastField) haengt reproduzierbar
                    // an exakt dieser Stelle, sowohl MIT als auch OHNE den unabhaengigen
                    // ScanExistingDataTables-Absturz. Warum ausgerechnet der VIERTE Aufruf
                    // derselben Funktion auf demselben Row-Struct haengt, nachdem die ersten drei
                    // (UnlockItemID, Product_Id, Material1_Id) im selben Lauf sauber durchliefen,
                    // ist NICHT geklaert -- kein Fehlersignal, keine weitere Trace-Zeile, Prozess/
                    // REST-API blieben am Leben. Passt zum Gesamtbild (siehe UnlockItemID-Befund
                    // oben): der DT_ItemRecipeDataTable-Row-Speicher fuer per AddRow neu angelegte
                    // Zeilen ist unter Linux nicht durchgaengig zuverlaessig, unabhaengig vom
                    // konkreten Lesepfad. Bewusst NICHT weiter live instrumentiert.
                }
                else
                {
                    PS::Trace("DogCoinRecipe: Row nicht in DT_ItemRecipeDataTable gefunden");
                }

                // Referenz-Rezepte suchen (2026-08-16c): der vorherige Versuch verglich
                // ueber den Zeilennamen mit DT_ItemDataTable -- das passte nur zufaellig bei
                // DogCoin (der Modder hat die Rezeptzeile absichtlich "DogCoin" genannt wie
                // das Item selbst). Bei 2466 durchsuchten Items kam so aber KEIN einziger
                // Treffer heraus, was zeigt, dass vanilla Rezeptzeilen nicht zuverlaessig so
                // heissen wie ihr Produkt-Item. Deshalb hier direkt auf DT_ItemRecipeDataTable
                // selbst iterieren (das ist die kleinere Tabelle -- nur Zeilen, die ueberhaupt
                // ein Rezept haben) und pro Zeile Product_Id + UnlockItemID lesen. Wieder nur
                // Einzelfelder per GetPropertyByName, kein Struct-Walk.
                auto* productIdProp = CastField<FNameProperty>(Palworld::PropertyHelper::GetPropertyByName(recipeRowStruct, STR("Product_Id")));
                if (!productIdProp)
                {
                    PS::Trace("Referenz-Rezepte: Product_Id-Property nicht gefunden");
                }
                else
                {
                    int checked = 0;
                    int shown = 0;
                    for (auto& Pair : recipeTable->GetRowMap())
                    {
                        ++checked;
                        if (shown >= 3 || checked > 5000) break;
                        if (Pair.Key == DogCoinRecipeRowName) continue; // eigener Datenpunkt, schon oben ausgegeben
                        void* refRecipeRow = Pair.Value;
                        if (!refRecipeRow) continue;

                        ++shown;
                        auto refRowName = RC::to_string(Pair.Key.ToString());
                        char label1[96];
                        char label2[96];
                        std::snprintf(label1, sizeof(label1), "RefRecipe[%s].Product_Id", refRowName.c_str());
                        std::snprintf(label2, sizeof(label2), "RefRecipe[%s].UnlockItemID", refRowName.c_str());
                        PropertyHelper::TraceScalarValue(label1, productIdProp, productIdProp->ContainerPtrToValuePtr<void>(refRecipeRow));
                        PropertyHelper::TraceScalarValue(label2, unlockProp, unlockProp->ContainerPtrToValuePtr<void>(refRecipeRow));
                    }
                    PS::Trace("Referenz-Rezepte: %d gezeigt (checked=%d)", shown, checked);
                }
            }
        }
        else
        {
            PS::Trace("DT_ItemRecipeDataTable: nicht gefunden (TryGetDatatableByName)");
        }

        // Messpunkt (2026-08-18): dritte Cache-Instanz-Hypothese direkt pruefen. Eine
        // Lua-Reflection-Sondierung (UE4SS ForEachFunction/ForEachProperty auf laufendem
        // Testserver, siehe Chat) fand PalMapObjectConvertItemModel als das eigentliche
        // Laufzeitobjekt hinter jeder Werkbank/Fliessband-Instanz -- nicht die
        // ItemConverterParameterComponent (die hat ausser generischen ActorComponent-
        // Funktionen keine eigene Logik, alles laeuft ueber Blueprint-ExecuteUbergraph).
        // Das Model hat eine EIGENE Kopie von TargetTypesA/TargetTypesB/TargetRankMax
        // (property-identisch zur Component, vermutlich beim Spawnen daraus kopiert) UND
        // eine eigene "RecipeIds"-ArrayProperty (FNameProperty) -- naheliegend die pro
        // Instanz gecachte, UI-sichtbare Rezeptliste. GetRecipes() (die Funktion, die laut
        // Namen und Aufrufkontext in WBP_PalConvertItemMenu_RecipeSlotButton/
        // WBP_MultiProductRecipeSelect die Liste liefert) konnte per Lua nicht direkt
        // aufgerufen werden (UE4SS' generischer Call-Marshaller unterstuetzt keinen
        // ArrayProperty-Rueckgabewert) -- deshalb hier stattdessen RecipeIds roh auslesen,
        // exakt wie TargetTypesA/B oben schon fuer die Component gemacht wurde.
        {
            static const FName ConvertItemModelClassName(STR("PalMapObjectConvertItemModel"));
            static const FName ConvertItemModelDefaultName(STR("Default__PalMapObjectConvertItemModel"));
            int32_t scanned = 0;

            UObjectGlobals::ForEachUObject([&](UObject* Object, ::RC::Unreal::int32, ::RC::Unreal::int32) {
                if (!Object) return RC::LoopAction::Continue;
                auto* Cls = Object->GetClassPrivate();
                if (!Cls) return RC::LoopAction::Continue;
                if (Cls->GetNamePrivate() != ConvertItemModelClassName) return RC::LoopAction::Continue;
                if (Object->GetNamePrivate() == ConvertItemModelDefaultName) return RC::LoopAction::Continue;

                ++scanned;
                if (scanned > 40) return RC::LoopAction::Continue; // Testworld hat ~37 Instanzen

                char rankLabel[96];
                std::snprintf(rankLabel, sizeof(rankLabel), "ConvertItemModel[%d].TargetRankMax", scanned);
                if (auto* rankProp = CastField<FNumericProperty>(Palworld::PropertyHelper::GetPropertyByName(Object->GetClassPrivate(), STR("TargetRankMax"))))
                {
                    PropertyHelper::TraceScalarValue(rankLabel, rankProp, rankProp->ContainerPtrToValuePtr<void>(Object));
                }
                else
                {
                    PS::Trace("%s: Property nicht gefunden", rankLabel);
                }

                char b_label[96];
                std::snprintf(b_label, sizeof(b_label), "ConvertItemModel[%d].TargetTypesB", scanned);
                PsTraceComponentArray(Object, STR("TargetTypesB"), b_label);

                char ids_label[96];
                std::snprintf(ids_label, sizeof(ids_label), "ConvertItemModel[%d].RecipeIds", scanned);
                PsTraceComponentArray(Object, STR("RecipeIds"), ids_label);

                return RC::LoopAction::Continue;
            });

            PS::Trace("ConvertItemModel-Scan: %d Instanzen geprueft", scanned);
        }

        // Messpunkt (2026-08-18) -- Fix: der Scan oben bewies den Fund (982 RecipeIds an der
        // Rang-10-Alle-Typen-Station, kein einziges DogCoin darunter): RecipeIds ist ein pro
        // Instanz gecachtes Ergebnis, das exakt einmal aus DT_ItemRecipeDataTable gefiltert
        // wurde -- vor dieser AddRow-Zeile. Der native Neuaufbau-Mechanismus (vermutlich
        // GetRecipes() auf PalMapObjectConvertItemModel) ist nicht per Lua/Hook erreichbar
        // (UE4SS' generischer Call-Marshaller unterstuetzt keinen ArrayProperty-Rueckgabewert,
        // siehe Chat). Statt ihn zu suchen: denselben Filter, den das Spiel selbst fuer jede
        // andere Zeile bereits sichtbar korrekt anwendet (TargetTypesA/TargetTypesB/
        // TargetRankMax gegen das Produkt-Item), hier fuer die vom Mod neu hinzugefuegten
        // Rezepte nachbilden und das Ergebnis direkt in RecipeIds nachtragen -- ueber
        // FScriptArrayHelper::Add(), die den Engine-Allokator bereits sicher handhabt (siehe
        // PS_ENGINE_ARRAY_GROW, verifiziert am Medaillen-Haendler-Fix).
        if (auto* fixItemTable = TryGetDatatableByName("DT_ItemDataTable"))
        {
            if (auto* fixRecipeTable = TryGetDatatableByName("DT_ItemRecipeDataTable"))
            {
                auto fixItemRowStruct = fixItemTable->GetRowStruct().Get();
                auto fixRecipeRowStruct = fixRecipeTable->GetRowStruct().Get();

                auto* typeAProp = fixItemRowStruct ? CastField<FEnumProperty>(Palworld::PropertyHelper::GetPropertyByName(fixItemRowStruct, STR("TypeA"))) : nullptr;
                auto* typeBProp = fixItemRowStruct ? CastField<FEnumProperty>(Palworld::PropertyHelper::GetPropertyByName(fixItemRowStruct, STR("TypeB"))) : nullptr;
                auto* rankProp = fixItemRowStruct ? CastField<FNumericProperty>(Palworld::PropertyHelper::GetPropertyByName(fixItemRowStruct, STR("Rank"))) : nullptr;
                auto* productIdProp = fixRecipeRowStruct ? CastField<FNameProperty>(Palworld::PropertyHelper::GetPropertyByName(fixRecipeRowStruct, STR("Product_Id"))) : nullptr;

                if (!typeAProp || !typeBProp || !rankProp || !productIdProp)
                {
                    PS::Trace("FixRecipeIds: benoetigte Properties nicht gefunden (TypeA=%p TypeB=%p Rank=%p Product_Id=%p)",
                        (void*)typeAProp, (void*)typeBProp, (void*)rankProp, (void*)productIdProp);
                }
                else
                {
                    // Bewusst eng gefasst auf die beiden von WanliSimplerDogCoin neu
                    // angelegten Zeilen, nicht generisch auf "alle per AddRow neuen Zeilen"
                    // -- ein generischer Fix muesste wissen, welche Zeilen ueberhaupt neu
                    // sind (die Loader tracken das aktuell nicht separat), und ist als
                    // eigenes, groesseres Vorhaben schon in den Projektnotizen vermerkt.
                    static const FName CandidateRecipes[] = { FName(STR("DogCoin")), FName(STR("DogCoinBulk100")) };
                    static const FName ConvertItemModelClassName(STR("PalMapObjectConvertItemModel"));
                    static const FName ConvertItemModelDefaultName(STR("Default__PalMapObjectConvertItemModel"));
                    int32_t totalAdded = 0;

                    for (const FName& recipeName : CandidateRecipes)
                    {
                        auto recipeNameStr = RC::to_string(recipeName.ToString());
                        auto* recipeRow = fixRecipeTable->FindRowUnchecked(recipeName);
                        if (!recipeRow)
                        {
                            PS::Trace("FixRecipeIds: Rezeptzeile '%s' nicht gefunden", recipeNameStr.c_str());
                            continue;
                        }

                        FName productId = productIdProp->GetPropertyValue(productIdProp->ContainerPtrToValuePtr<void>(recipeRow));
                        auto* itemRow = fixItemTable->FindRowUnchecked(productId);
                        if (!itemRow)
                        {
                            PS::Trace("FixRecipeIds: Produktzeile '%s' nicht in DT_ItemDataTable", RC::to_string(productId.ToString()).c_str());
                            continue;
                        }

                        int64_t wantTypeA = 0, wantTypeB = 0, wantRank = 0;
                        PsReadRawIntegral(typeAProp, typeAProp->ContainerPtrToValuePtr<void>(itemRow), wantTypeA);
                        PsReadRawIntegral(typeBProp, typeBProp->ContainerPtrToValuePtr<void>(itemRow), wantTypeB);
                        PsReadRawIntegral(rankProp, rankProp->ContainerPtrToValuePtr<void>(itemRow), wantRank);
                        PS::Trace("FixRecipeIds: '%s' -> Produkt='%s' TypeA=%lld TypeB=%lld Rank=%lld",
                            recipeNameStr.c_str(), RC::to_string(productId.ToString()).c_str(),
                            (long long)wantTypeA, (long long)wantTypeB, (long long)wantRank);

                        int32_t scanned = 0;
                        int32_t appended = 0;

                        UObjectGlobals::ForEachUObject([&](UObject* Object, ::RC::Unreal::int32, ::RC::Unreal::int32) {
                            if (!Object) return RC::LoopAction::Continue;
                            auto* Cls = Object->GetClassPrivate();
                            if (!Cls || Cls->GetNamePrivate() != ConvertItemModelClassName) return RC::LoopAction::Continue;
                            if (Object->GetNamePrivate() == ConvertItemModelDefaultName) return RC::LoopAction::Continue;
                            ++scanned;

                            // Messpunkt (2026-08-18, Nachtrag): der erste Versuch gate'te hier
                            // zusaetzlich auf PsArrayContainsRawValue(TargetTypesA/TargetTypesB)
                            // -- Ergebnis 0/35 fuer BEIDE Kandidatenrezepte, ausnahmslos, auch bei
                            // der Rang-10-Station mit 37 (von insgesamt vermutlich 38) Eintraegen
                            // in TargetTypesB. Das beweist: MaterialJewelry (Rohwert 24) fehlt in
                            // JEDER live Model-Instanz, unabhaengig von Rang/Breite -- die vom
                            // Blueprint-Loader nachweislich korrekt gepatchte
                            // ItemConverterParameterComponent.TargetTypesB (siehe
                            // ApplyToExistingObjects oben, dort schon vor dieser Sitzung
                            // verifiziert) erreicht das Model also gar nicht, ueber KEINEN
                            // beobachtbaren Pfad -- nicht nur "veraltet", sondern strukturell
                            // entkoppelt. Der Typ-Abgleich ist damit als Gate fuer RecipeIds
                            // nachweislich untauglich. Ersatzweise auf die beiden vom Mod
                            // gemeinten Stationen ueber ihren TargetRankMax zielen -- 10 fuer
                            // BP_BuildObject_AncientWorkBench_C, 5 fuer
                            // BP_BuildObject_Factory_Hard_4_C, beide unabhaengig in einer
                            // frueheren Sitzung direkt an den jeweiligen Components verifiziert
                            // (siehe palschema-todo.md, Kandidat 4). Bewusste Ungenauigkeit: sollte
                            // eine dritte, unverwandte Stationsart zufaellig denselben Rang
                            // benutzen, bekommt auch sie das Rezept -- fuer den Nachweis "Mod
                            // funktioniert Ende-zu-Ende" hinnehmbar, im Bericht offengelegt.
                            auto* instRankProp = CastField<FNumericProperty>(Palworld::PropertyHelper::GetPropertyByName(Object->GetClassPrivate(), STR("TargetRankMax")));
                            if (!instRankProp) return RC::LoopAction::Continue;
                            int64_t instRank = 0;
                            if (!PsReadRawIntegral(instRankProp, instRankProp->ContainerPtrToValuePtr<void>(Object), instRank)) return RC::LoopAction::Continue;
                            if (instRank != 10 && instRank != 5) return RC::LoopAction::Continue;

                            if (PsRecipeIdsContains(Object, recipeName)) return RC::LoopAction::Continue; // schon drin

                            auto rawProp = Palworld::PropertyHelper::GetPropertyByName(Object->GetClassPrivate(), STR("RecipeIds"));
                            auto arrProp = CastField<FArrayProperty>(rawProp);
                            if (!arrProp) return RC::LoopAction::Continue;
                            auto dataPtr = arrProp->ContainerPtrToValuePtr<void>(Object);
                            auto scriptArray = static_cast<FScriptArray*>(dataPtr);
                            UECustom::FScriptArrayHelper helper(scriptArray, arrProp);
                            FName nameCopy = recipeName;
                            helper.Add(&nameCopy);
                            ++appended;

                            // Messpunkt (2026-08-18): Rueckgelesen, nicht nur geloggt, dass die
                            // Add() ohne Fehler durchlief -- exakt die Lehre, die dieses Projekt
                            // zweimal teuer gelernt hat (Composite-Tabellen, AddRow): eine
                            // Erfolgsmeldung ist kein Wirkungsnachweis.
                            const bool verified = PsRecipeIdsContains(Object, recipeName);
                            PS::Trace("FixRecipeIds:   Instanz %p Rueckgelesen nach Add(): %s",
                                (void*)Object, verified ? "DogCoin/DogCoinBulk100 jetzt enthalten" : "FEHLT WEITERHIN NACH ADD -- Add() wirkungslos");

                            return RC::LoopAction::Continue;
                        });

                        PS::Trace("FixRecipeIds: '%s' -> %d/%d Instanzen ergaenzt", recipeNameStr.c_str(), appended, scanned);
                        totalAdded += appended;
                    }

                    PS::Log<RC::LogLevel::Normal>(STR("FixRecipeIds: {} RecipeIds-Eintraege insgesamt nachgetragen.\n"), totalAdded);
                }
            }
            else
            {
                PS::Trace("FixRecipeIds: DT_ItemRecipeDataTable nicht gefunden");
            }
        }
        else
        {
            PS::Trace("FixRecipeIds: DT_ItemDataTable nicht gefunden");
        }

        // Messpunkt (2026-08-18, Nachtrag): Besitzer meldet, das Rezept sei trotz 16/16
        // verifizierter Server-Schreibung -- auch an einer NEU gebauten Station -- im Spiel
        // weiterhin unsichtbar. Klassisches Signatur eines Replikations- statt
        // Datenproblems: Server-Speicher stimmt nachweislich, Client sieht es nicht.
        // Zwei Kandidaten: (a) RecipeIds ist repliziert, aber unser roher Speicherschreiber
        // (FScriptArrayHelper::Add) geht komplett an der Replikations-Buchhaltung vorbei
        // (kein MARK_PROPERTY_DIRTY, kein regulaerer Setter) -- push-model-Replikation wuerde
        // die Aenderung dann nie zum naechsten Replikationsdurchlauf anmelden. (b) RecipeIds
        // ist GAR NICHT repliziert und wird auf Client UND Server unabhaengig aus denselben,
        // lokal identischen (vanilla) Content-Dateien berechnet -- dann waere unser rein
        // serverseitiger DataTable-Patch fuer diese Liste von vornherein wirkungslos, egal wie
        // korrekt er im Server-Speicher steht, weil der Client ihn nie sieht.
        // Unterscheidung ueber die Property-Flags selbst: CPF_Net gesetzt? Referenzvergleich
        // gegen CurrentRecipeId (hat nachweislich OnRep_CurrentRecipeId, muss also CPF_Net
        // haben) und TargetTypesB (vermutlich statische Konstruktionsdaten, kein Net).
        {
            // Einfacher als eine eigene UClass-Objektsuche: eine lebende Instanz wie im
            // Fix-Block oben finden und ihre Klasse direkt abfragen -- dasselbe
            // GetClassPrivate()/GetNamePrivate()-Muster, das im ganzen Modul schon
            // zuverlaessig verwendet wird.
            static const FName ConvertItemModelClassName2(STR("PalMapObjectConvertItemModel"));
            static const FName ConvertItemModelDefaultName2(STR("Default__PalMapObjectConvertItemModel"));
            RC::Unreal::UClass* modelClass = nullptr;
            UObjectGlobals::ForEachUObject([&](UObject* Object, ::RC::Unreal::int32, ::RC::Unreal::int32) {
                if (modelClass) return RC::LoopAction::Continue;
                if (!Object) return RC::LoopAction::Continue;
                auto* Cls = Object->GetClassPrivate();
                if (!Cls || Cls->GetNamePrivate() != ConvertItemModelClassName2) return RC::LoopAction::Continue;
                if (Object->GetNamePrivate() == ConvertItemModelDefaultName2) return RC::LoopAction::Continue;
                modelClass = Cls;
                return RC::LoopAction::Continue;
            });

            if (!modelClass)
            {
                PS::Trace("ReplCheck: PalMapObjectConvertItemModel-Klasse nicht gefunden");
            }
            else
            {
                auto TraceFlags = [&](const RC::StringType& propName) {
                    auto propNameStr = RC::to_string(propName);
                    auto* prop = Palworld::PropertyHelper::GetPropertyByName(modelClass, propName);
                    if (!prop)
                    {
                        PS::Trace("ReplCheck: Property '%s' nicht gefunden", propNameStr.c_str());
                        return;
                    }
                    auto flags = prop->GetPropertyFlags();
                    const bool isNet = (flags & CPF_Net) != 0;
                    const bool isRepNotify = (flags & CPF_RepNotify) != 0;
                    const bool isRepSkip = (flags & CPF_RepSkip) != 0;
                    PS::Trace("ReplCheck: '%s' Flags=0x%llx CPF_Net=%s CPF_RepNotify=%s CPF_RepSkip=%s",
                        propNameStr.c_str(), (unsigned long long)flags,
                        isNet ? "JA" : "nein", isRepNotify ? "JA" : "nein", isRepSkip ? "JA" : "nein");
                };

                TraceFlags(STR("RecipeIds"));
                TraceFlags(STR("CurrentRecipeId"));        // Referenz: hat OnRep_CurrentRecipeId, muss CPF_Net haben
                TraceFlags(STR("RequestedProductNum"));     // Referenz: hat OnRep_RequestedProductNum
                TraceFlags(STR("bIsWorkable"));              // Referenz: hat OnRep_IsWorkable
                TraceFlags(STR("TargetTypesA"));             // Vergleich: vermutlich statisch, kein Net
                TraceFlags(STR("TargetTypesB"));             // Vergleich: vermutlich statisch, kein Net
                TraceFlags(STR("TargetRankMax"));            // Vergleich: vermutlich statisch, kein Net
            }
        }

        // Zusatzcheck: repliziert die BESITZENDE Actor-Klasse (AncientWorkBench) ueberhaupt?
        // Vorbedingung dafuer, dass irgendeine ihrer Component-/Subobjekt-Properties je
        // repliziert -- unabhaengig vom Ergebnis oben.
        {
            static const FName AncientWorkBenchName(STR("BP_BuildObject_AncientWorkBench_C"));
            RC::Unreal::UClass* wbClass = nullptr;
            UObjectGlobals::ForEachUObject([&](UObject* Object, ::RC::Unreal::int32, ::RC::Unreal::int32) {
                if (wbClass) return RC::LoopAction::Continue;
                if (!Object) return RC::LoopAction::Continue;
                auto* Cls = Object->GetClassPrivate();
                if (!Cls || Cls->GetNamePrivate() != AncientWorkBenchName) return RC::LoopAction::Continue;
                wbClass = Cls;
                return RC::LoopAction::Continue;
            });

            if (!wbClass)
            {
                PS::Trace("ReplCheck: BP_BuildObject_AncientWorkBench_C-Klasse nicht gefunden");
            }
            else if (auto* replProp = CastField<FBoolProperty>(Palworld::PropertyHelper::GetPropertyByName(wbClass, STR("bReplicates"))))
            {
                auto& cdo = wbClass->GetClassDefaultObject();
                if (cdo.Get())
                {
                    PS::Trace("ReplCheck: BP_BuildObject_AncientWorkBench_C.bReplicates (CDO) = %s",
                        replProp->GetPropertyValue(replProp->ContainerPtrToValuePtr<void>(cdo.Get())) ? "true" : "false");
                }
            }
            else
            {
                PS::Trace("ReplCheck: bReplicates-Property nicht gefunden");
            }
        }
    }
#endif

    void PalBlueprintModLoader::PostLoad(RC::Unreal::UClass* self)
    {
        PostLoadHook.call(self);

        if (!PostLoadCallback)
        {
            return;
        }

        PostLoadCallback(self);
    }

    void PalBlueprintModLoader::PostInitComponents(RC::Unreal::AActor* self)
    {
        PostInitComponentsHook.call(self);

        if (!PostInitComponentsCallback)
        {
            return;
        }

        PostInitComponentsCallback(self);
    }
}