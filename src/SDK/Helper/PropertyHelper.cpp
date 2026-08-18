#include <cstdio>
#include "Utility/LinuxFormat.h"
#include "Unreal/FProperty.hpp"
#include "Unreal/Property/FEnumProperty.hpp"
#include "Unreal/Property/FStrProperty.hpp"
#include "Unreal/Property/FTextProperty.hpp"
#include "Unreal/CoreUObject/UObject/UnrealType.hpp"
#include "Unreal/CoreUObject/UObject/Class.hpp"
#include "Helpers/Casting.hpp"
#include "SDK/Classes/TSoftObjectPtr.h"
#include "SDK/Classes/TSoftClassPtr.h"
#include "SDK/Classes/KismetSystemLibrary.h"
#include "SDK/Structs/Custom/FManagedValue.h"
#include "SDK/Structs/Custom/FScriptMapHelper.h"
#include "SDK/Structs/Custom/FScriptArrayHelper.h"
#include "SDK/Helper/PropertyHelper.h"
#include "SDK/PalSignatures.h"
#include "Utility/Logging.h"
#include "Utility/PsTrace.h"

using namespace RC;
using namespace RC::Unreal;


#ifdef __linux__
// ---------------------------------------------------------------------------
// Linux-Portierung: Korrektur der UE4SS-vtable-Offsets
//
// UE4SS ruft Engine-Virtuals ueber fest verdrahtete Byte-Offsets aus
// VTableLayoutMap auf (siehe UnrealVirtualBaseVC.hpp). Diese Offsets stammen
// aus MSVC-Builds. Unter dem Itanium-C++-ABI (Linux/Clang) hat jede polymorphe
// Klasse ZWEI Destruktor-Slots (D1 = complete object, D0 = deleting), MSVC nur
// einen (__vecDelDtor). Dadurch sind ALLE UE4SS-Offsets unter Linux um genau
// 8 Byte (einen Slot) zu klein.
//
// Verifiziert an PalServer-Linux-Shipping:
//   _ZTV9FProperty      = 368 Byte => 46 Slots (0x170)
//   UE4SS FProperty-Map endet bei SameType = 0x160 => 45 Slots (0x168)
//   _ZTV12FIntProperty / _ZTV15FDoubleProperty / _ZTV13FByteProperty:
//     Slot 0x180 ist bei Int/Double/Int64 identisch, nur bei Byte anders
//     => das ist GetIntPropertyEnum (nur FByteProperty ueberschreibt es).
//     UE4SS sucht es bei 0x178 -> das ist real IsInteger.
//
// Folge ohne Fix: FNumericProperty::IsEnum() -> GetIntPropertyEnum() ruft in
// Wahrheit IsInteger() auf. Das liefert bool in AL, die oberen 56 Bit von RAX
// bleiben Muell -> als UEnum* gelesen ist der Wert != nullptr -> IsEnum() ist
// faelschlich true.
// ---------------------------------------------------------------------------
namespace {
    // MSVC-Offset + 8
    constexpr unsigned PS_VT_FNumeric_GetIntPropertyEnum = 0x180; // UE4SS: 0x178

    template <typename Fn>
    Fn PsResolveVirtual(const void* Self, unsigned Offset)
    {
        auto* VTable = *reinterpret_cast<void* const*>(Self);
        return reinterpret_cast<Fn>(*reinterpret_cast<void* const*>(
            reinterpret_cast<const char*>(VTable) + Offset));
    }

    RC::Unreal::UEnum* PsGetIntPropertyEnum(RC::Unreal::FNumericProperty* Property)
    {
        using Fn = RC::Unreal::UEnum* (*)(const void*);
        return PsResolveVirtual<Fn>(Property, PS_VT_FNumeric_GetIntPropertyEnum)(Property);
    }

    // Numerische Klassifikation ohne jeden virtuellen Aufruf: die FFieldClass
    // ist ueber nicht-virtuelle Member erreichbar und unter Linux korrekt.
    enum class PsNumericKind { Unknown, Integer, FloatingPoint, ByteEnum };

    PsNumericKind PsClassifyNumeric(RC::Unreal::FNumericProperty* Property)
    {
        auto FieldClass = Property->GetClass();
        auto ClassName = FieldClass.GetName();

        if (ClassName == STR("FloatProperty") || ClassName == STR("DoubleProperty"))
        {
            return PsNumericKind::FloatingPoint;
        }
        if (ClassName == STR("ByteProperty"))
        {
            // Nur ein ByteProperty kann ein Enum tragen (FByteProperty::Enum).
            return PsGetIntPropertyEnum(Property) ? PsNumericKind::ByteEnum : PsNumericKind::Integer;
        }
        if (ClassName == STR("Int8Property") || ClassName == STR("Int16Property")
            || ClassName == STR("IntProperty") || ClassName == STR("Int64Property")
            || ClassName == STR("UInt16Property") || ClassName == STR("UInt32Property")
            || ClassName == STR("UInt64Property"))
        {
            return PsNumericKind::Integer;
        }
        return PsNumericKind::Unknown;
    }
}
#endif

namespace Palworld {
    void PropertyHelper::CopyJsonValueToContainer(void* Container, FProperty* Property, const nlohmann::json& Value)
    {
        if (!Property)
        {
            throw std::runtime_error("A null Property was supplied to PropertyHelper::CopyJsonValueToContainer.");
        }

        auto PropertyName = Property->GetName();
#ifndef __linux__
        auto Type = Property->GetCPPType();
#endif
        auto Class = Property->GetClass();
        auto ClassName = Class.GetName();
        auto ValuePtr = Property->ContainerPtrToValuePtr<void>(Container);


        if (auto EnumProperty = CastProperty<FEnumProperty>(Property))
        {
            SetEnumPropertyValueFromJsonValue(ValuePtr, EnumProperty, Value);
        }
        else if (auto NumProperty = CastProperty<FNumericProperty>(Property))
        {
            SetNumericPropertyValueFromJsonValue(ValuePtr, NumProperty, Value);
        }
        else if (auto BoolProperty = CastProperty<FBoolProperty>(Property))
        {
            SetBoolPropertyValueFromJsonValue(ValuePtr, BoolProperty, Value);
        }
        else if (auto NameProperty = CastProperty<FNameProperty>(Property))
        {
            SetNamePropertyValueFromJsonValue(ValuePtr, NameProperty, Value);
        }
        else if (auto StrProperty = CastProperty<FStrProperty>(Property))
        {
            SetStrPropertyValueFromJsonValue(ValuePtr, StrProperty, Value);
        }
        else if (auto TextProperty = CastProperty<FTextProperty>(Property))
        {
            SetTextPropertyValueFromJsonValue(ValuePtr, TextProperty, Value);
        }
        else if (auto ClassProperty = CastProperty<FClassProperty>(Property))
        {
            SetClassPropertyValueFromJsonValue(ValuePtr, ClassProperty, Value);
        }
        else if (CastProperty<FObjectProperty>(Property) && ClassName == STR("ObjectProperty"))
        {
            auto ObjectProperty = CastProperty<FObjectProperty>(Property);
            SetObjectPropertyValueFromJsonValue(Container, ObjectProperty, Value);
        }
        else if (CastProperty<FSoftObjectProperty>(Property) && ClassName == STR("SoftObjectProperty"))
        {
            auto SoftObjectProperty = CastProperty<FSoftObjectProperty>(Property);
            SetSoftObjectPropertyValueFromJsonValue(ValuePtr, SoftObjectProperty, Value);
        }
        else if (CastProperty<FSoftClassProperty>(Property) && ClassName == STR("SoftClassProperty"))
        {
            auto SoftClassProperty = CastProperty<FSoftClassProperty>(Property);
            SetSoftClassPropertyValueFromJsonValue(ValuePtr, SoftClassProperty, Value);
        }
        else if (auto StructProperty = CastProperty<FStructProperty>(Property))
        {
            SetStructPropertyValueFromJsonValue(ValuePtr, StructProperty, Value);
        }
        else if (auto ArrayProperty = CastProperty<FArrayProperty>(Property))
        {
            SetArrayPropertyValueFromJsonValue(ValuePtr, ArrayProperty, Value);
        }
        else if (auto MapProperty = CastProperty<FMapProperty>(Property))
        {
            SetMapPropertyValueFromJsonValue(ValuePtr, MapProperty, Value);
        }
        else
        {
#ifdef __linux__
            PS::Log<RC::LogLevel::Warning>(STR("Unhandled property '{}' with class of {}\n"), PropertyName, ClassName);
#else
            PS::Log<RC::LogLevel::Warning>(STR("Unhandled property '{}' with class of {} and type of {}\n"), PropertyName, ClassName, Type.GetCharArray());
#endif
        }
    }

    // Messpunkt Werkbank-Rezept (2026-08-15): siehe PropertyHelper.h. Absichtlich ohne
    // jede eigene Ausnahme/Exception -- ein throw ist in diesem Prozess toedlich, und eine
    // Diagnosefunktion darf niemals selbst zur Absturzursache werden (siehe Learning 9:
    // "Bei einem Absturz zuerst pruefen, ob es wirklich fremder Code ist").
    void PropertyHelper::TraceScalarValue(const char* Label, FProperty* Property, void* Data)
    {
        if (!Property || !Data)
        {
            PS::Trace("%s: Property oder Data ist null", Label);
            return;
        }

        auto ClassName = RC::to_string(Property->GetClass().GetName());

        // FBoolProperty::GetPropertyValue ist FORCEINLINE und liest nur das Bitfeld direkt --
        // kein virtueller Aufruf, also ohne die Linux-Offset-Problematik der Numeric-Properties.
        if (auto BoolProperty = CastProperty<FBoolProperty>(Property))
        {
            PS::Trace("%s: Klasse=BoolProperty Wert=%s", Label, BoolProperty->GetPropertyValue(Data) ? "true" : "false");
            return;
        }

        // FNameProperty::GetPropertyValue kommt aus TPropertyTypeFundamentals (FORCEINLINE,
        // statisch, nicht-virtuell) -- derselbe sichere Zugriffsweg wie bei Bool oben.
        if (auto NameProperty = CastProperty<FNameProperty>(Property))
        {
            auto NameValue = RC::to_string(NameProperty->GetPropertyValue(Data).ToString());
            PS::Trace("%s: Klasse=NameProperty Wert='%s'", Label, NameValue.c_str());
            return;
        }

        const bool bIsEnum = (ClassName == "EnumProperty");
        const bool bIsByte = (ClassName == "ByteProperty");
        const bool bIsOtherNumeric = !bIsByte && CastProperty<FNumericProperty>(Property) != nullptr;

        if (!bIsEnum && !bIsByte && !bIsOtherNumeric)
        {
            PS::Trace("%s: uebersprungen, Klasse=%s ist nicht enum-/zahlenartig", Label, ClassName.c_str());
            return;
        }

        auto Size = Property->GetElementSize();
        int64 Raw = 0;
        switch (Size)
        {
        case 1: Raw = *static_cast<uint8*>(Data);  break;
        case 2: Raw = *static_cast<uint16*>(Data); break;
        case 4: Raw = *static_cast<uint32*>(Data); break;
        case 8: Raw = *static_cast<int64*>(Data);  break;
        default:
            PS::Trace("%s: Klasse=%s hat unerwartete Groesse %d", Label, ClassName.c_str(), static_cast<int>(Size));
            return;
        }

        UEnum* Enum = nullptr;
        const char* EnumSource = "kein Enum";

        if (bIsEnum)
        {
            auto EnumProperty = CastProperty<FEnumProperty>(Property);
            Enum = EnumProperty->GetEnum().Get();
            EnumSource = "FEnumProperty::GetEnum, nicht-virtuell";
        }
        else if (bIsByte)
        {
            auto NumProperty = CastProperty<FNumericProperty>(Property);
#ifdef __linux__
            Enum = PsGetIntPropertyEnum(NumProperty);
            EnumSource = "PsGetIntPropertyEnum @0x180, UNVERIFIZIERT unter Linux";
#else
            Enum = NumProperty->GetIntPropertyEnum();
            EnumSource = "GetIntPropertyEnum, virtuell (Windows)";
#endif
        }

        if (!Enum)
        {
            PS::Trace("%s: Klasse=%s Rohwert=%lld kein Enum aufgeloest (Quelle: %s)", Label, ClassName.c_str(), static_cast<long long>(Raw), EnumSource);
            return;
        }

        for (const auto& EnumPair : Enum->GetEnumNames())
        {
            if (EnumPair.Value == Raw)
            {
                auto SymbolName = RC::to_string(EnumPair.Key.ToString());
                PS::Trace("%s: Klasse=%s Rohwert=%lld Symbol=%s (Quelle: %s)", Label, ClassName.c_str(), static_cast<long long>(Raw), SymbolName.c_str(), EnumSource);
                return;
            }
        }

        auto EnumTypeName = RC::to_string(Enum->GetName());
        PS::Trace("%s: Klasse=%s Rohwert=%lld Symbol=<kein Treffer in %s> (Quelle: %s)", Label, ClassName.c_str(), static_cast<long long>(Raw), EnumTypeName.c_str(), EnumSource);
    }

    int64 PropertyHelper::ParseEnumFromJsonValue(FEnumProperty* Property, const nlohmann::json& Value)
    {
        auto PropertyName = GetPropertyNameAsUTF8String(Property);

        ValidateJsonValueType(Property, Value);

        auto Enum = Property->GetEnum();
        if (!Enum)
        {
            throw std::runtime_error(std::format("EnumProperty {} had an invalid Enum value", PropertyName));
        }

        // Praefix aus dem UEnum-Objektnamen statt aus GetCPPType() (siehe
        // GetPropertyTypeAsUTF8String): "EPalItemShopProductType".
        auto PropertyType = RC::to_string(Enum->GetName());

        auto ParsedValue = Value.get<std::string>();
        if (!ParsedValue.contains("::"))
        {
            ParsedValue = std::format("{}::{}", PropertyType, ParsedValue);
        }

        auto EnumName = FName(RC::to_generic_string(ParsedValue));

        bool WasEnumFound = false;
        int64_t EnumValue = 0;

        for (const auto& EnumPair : Enum->GetEnumNames())
        {
            if (EnumPair.Key == EnumName)
            {
                WasEnumFound = true;
                EnumValue = EnumPair.Value;
            }
        }

        if (!WasEnumFound)
        {
            throw std::runtime_error(std::format("Enum '{}' doesn't exist", ParsedValue));
        }

        return EnumValue;
    }

    int64 PropertyHelper::ParseByteFromJsonValue(FNumericProperty* Property, const nlohmann::json& Value)
    {
        auto PropertyName = GetPropertyNameAsUTF8String(Property);
#ifdef __linux__
        auto Enum = PsGetIntPropertyEnum(Property);
#else
        auto Enum = Property->GetIntPropertyEnum();
#endif
        if (!Enum)
        {
            throw std::runtime_error(std::format("EnumProperty {} had an invalid Enum value", PropertyName));
        }

        auto PropertyType = RC::to_string(Enum->GetName());

        auto ParsedValue = Value.get<std::string>();
        if (!ParsedValue.contains("::"))
        {
            ParsedValue = std::format("{}::{}", PropertyType, ParsedValue);
        }

        auto EnumName = FName(RC::to_generic_string(ParsedValue));

        bool WasEnumFound = false;
        int64_t EnumValue = 0;

        for (const auto& EnumPair : Enum->GetEnumNames())
        {
            if (EnumPair.Key == EnumName)
            {
                WasEnumFound = true;
                EnumValue = EnumPair.Value;
            }
        }

        if (!WasEnumFound)
        {
            throw std::runtime_error(std::format("Enum '{}' doesn't exist", ParsedValue));
        }

        return EnumValue;
    }

    void PropertyHelper::SetEnumPropertyValueFromJsonValue(void* Data, FEnumProperty* Property, const nlohmann::json& Value)
    {
        auto EnumValue = ParseEnumFromJsonValue(Property, Value);
        FMemory::Memcpy(Data, &EnumValue, Property->GetElementSize());
    }

    void PropertyHelper::SetNumericPropertyValueFromJsonValue(void* Data, RC::Unreal::FNumericProperty* Property, const nlohmann::json& Value)
    {
        auto PropertyName = GetPropertyNameAsUTF8String(Property);
#ifdef __linux__
        const auto PsKind = PsClassifyNumeric(Property);
        const bool bPsIsEnum = (PsKind == PsNumericKind::ByteEnum);
#else
        const bool bPsIsEnum = Property->IsEnum();
#endif
        if (!bPsIsEnum)
        {
            ValidateJsonValueType(Property, Value);
        }

        if (bPsIsEnum)
        {
            auto EnumValue = ParseByteFromJsonValue(Property, Value);
#ifdef __linux__
            // SetIntPropertyValue ist ebenfalls ein Virtual mit MSVC-Offset
            // (und die beiden Overloads int64/uint64 sind unter Itanium
            // zusaetzlich vertauscht) -> direkt schreiben.
            FMemory::Memcpy(Data, &EnumValue, Property->GetElementSize());
#else
            Property->SetIntPropertyValue(Data, EnumValue);
#endif
        }
        else
        {
#ifdef __linux__
            const bool bPsIsInt = (PsKind == PsNumericKind::Integer);
#else
            const bool bPsIsInt = Property->IsInteger();
#endif
            if (bPsIsInt)
            {
#ifdef __linux__
                // Linux-Test: SetIntPropertyValue ist eine virtuelle Engine-Methode.
                // Stimmt deren vtable-Index unter Linux nicht, wird eine falsche
                // Funktion aufgerufen. Deshalb hier direkt anhand der Elementgroesse
                // schreiben — umgeht den virtuellen Aufruf vollstaendig.
                const auto Size = Property->GetElementSize();
                const int64 Raw = Value.get<int64>();
                switch (Size)
                {
                case 1: *static_cast<int8*>(Data)  = static_cast<int8>(Raw);  break;
                case 2: *static_cast<int16*>(Data) = static_cast<int16>(Raw); break;
                case 4: *static_cast<int32*>(Data) = static_cast<int32>(Raw); break;
                case 8: *static_cast<int64*>(Data) = Raw;                     break;
                default: Property->SetIntPropertyValue(Data, Raw);            break;
                }
#else
                Property->SetIntPropertyValue(Data, Value.get<int64>());
#endif
            }
#ifdef __linux__
            else if (PsKind == PsNumericKind::FloatingPoint)
            {
                // SetFloatingPointPropertyValue ist ebenfalls ein Virtual mit
                // falschem Offset -> direkt anhand der Elementgroesse schreiben.
                const auto Size = Property->GetElementSize();
                const double Raw = Value.get<double>();
                if (Size == 4)      { *static_cast<float*>(Data)  = static_cast<float>(Raw); }
                else if (Size == 8) { *static_cast<double*>(Data) = Raw; }
                else                { Property->SetFloatingPointPropertyValue(Data, Raw); }
            }
#else
            else if (Property->IsFloatingPoint())
            {
                Property->SetFloatingPointPropertyValue(Data, Value.get<double>());
            }
#endif
            else
            {
                PS::Log<RC::LogLevel::Warning>(STR("Unhandled Numeric Type: {}\n"), Property->GetName());
            }
        }
    }

    void PropertyHelper::SetBoolPropertyValueFromJsonValue(void* Data, RC::Unreal::FBoolProperty* Property, const nlohmann::json& Value)
    {
        ValidateJsonValueType(Property, Value);

        Property->SetPropertyValue(Data, Value.get<bool>());
    }

    void PropertyHelper::SetNamePropertyValueFromJsonValue(void* Data, RC::Unreal::FNameProperty* Property, const nlohmann::json& Value)
    {
        ValidateJsonValueType(Property, Value);

        auto ParsedValue = Value.get<std::string>();
        auto Name = FName(RC::to_generic_string(ParsedValue), FNAME_Add);
        Property->SetPropertyValue(Data, Name);
    }

    void PropertyHelper::SetStrPropertyValueFromJsonValue(void* Data, RC::Unreal::FStrProperty* Property, const nlohmann::json& Value)
    {
        ValidateJsonValueType(Property, Value);

        auto ParsedValue = Value.get<std::string>();
        auto String = FString(RC::to_generic_string(ParsedValue).c_str());
        Property->SetPropertyValue(Data, String);
    }

    void PropertyHelper::SetTextPropertyValueFromJsonValue(void* Data, RC::Unreal::FTextProperty* Property, const nlohmann::json& Value)
    {
        ValidateJsonValueType(Property, Value);

        auto StringValue = Value.get<std::string>();
        auto Text = FText(RC::to_generic_string(StringValue).c_str());
        Property->SetPropertyValue(Data, Text);
    }

    void PropertyHelper::SetClassPropertyValueFromJsonValue(void* Data, RC::Unreal::FClassProperty* Property, const nlohmann::json& Value)
    {
        ValidateJsonValueType(Property, Value);

        auto PropertyName = GetPropertyNameAsUTF8String(Property);

        auto StringValue = Value.get<std::string>();
        auto StringValueWide = RC::to_generic_string(StringValue);
        auto SoftObjectPtr = UECustom::TSoftObjectPtr<UObject>(UECustom::FSoftObjectPath(StringValueWide));
        auto Asset = UECustom::UKismetSystemLibrary::LoadAsset_Blocking(SoftObjectPtr);

        if (!Asset)
        {
            throw std::runtime_error(std::format("Property {} was supplied an invalid class of {}", PropertyName, StringValue));
        }

        Property->SetPropertyValue(Data, Asset);
    }

    void PropertyHelper::SetObjectPropertyValueFromJsonValue(void* Data, RC::Unreal::FObjectProperty* Property, const nlohmann::json& Value)
    {
        ValidateJsonValueType(Property, Value);

        if (Value.is_string())
        {
            auto StringValue = Value.get<std::string>();
            auto WideStringValue = RC::to_generic_string(StringValue);
            auto LoadedObject = UECustom::UKismetSystemLibrary::LoadAsset_Blocking(WideStringValue, true);
            if (!LoadedObject)
            {
                throw std::runtime_error(RC::fmt("Unable to apply changes to %S. Asset was invalid.", Property->GetName().c_str()));
            }

            auto& ExpectedClass = Property->GetPropertyClass();
            if (!LoadedObject->IsA(ExpectedClass))
            {
                throw std::runtime_error(RC::fmt(
                    "Unable to apply changes to %S. Asset didn't match the expected class for this property. Expected '%S', got '%S'", 
                    Property->GetName().c_str(),
                    ExpectedClass->GetName().c_str(),
                    LoadedObject->GetClassPrivate()->GetName().c_str())
                );
            }

            *Property->ContainerPtrToValuePtr<UObject*>(Data) = LoadedObject;
        }
        else if (Value.is_object())
        {
            auto ObjectValue = *Property->ContainerPtrToValuePtr<UObject*>(Data);
            auto ParsedValue = Value.get<nlohmann::json>();
            if (ObjectValue)
            {
                for (auto& [InnerKey, InnerValue] : Value.items())
                {
                    auto ObjectValue_PropertyName = RC::to_generic_string(InnerKey);
                    auto ObjectValue_Property = ObjectValue->GetPropertyByNameInChain(ObjectValue_PropertyName.c_str());

                    if (!ObjectValue_Property)
                    {
                        ObjectValue_Property = Palworld::PropertyHelper::GetPropertyByName(ObjectValue->GetClassPrivate(), ObjectValue_PropertyName);
                    }

                    if (ObjectValue_Property)
                    {
                        CopyJsonValueToContainer(ObjectValue, ObjectValue_Property, InnerValue);
                    }
                }
            }
        }
    }

    void PropertyHelper::SetSoftClassPropertyValueFromJsonValue(void* Data, RC::Unreal::FSoftClassProperty* Property, const nlohmann::json& Value)
    {
        ValidateJsonValueType(Property, Value);

        auto ParsedValue = Value.get<std::string>();
        auto String = RC::to_generic_string(ParsedValue);
        if (!String.ends_with(STR("_C"))) String += STR("_C");

        auto SoftClassPtr = UECustom::TSoftClassPtr<UClass>(UECustom::FSoftObjectPath(String));
        FMemory::Memcpy(Data, &SoftClassPtr, sizeof(UECustom::TSoftClassPtr<UClass>));
    }

    void PropertyHelper::SetSoftObjectPropertyValueFromJsonValue(void* Data, RC::Unreal::FSoftObjectProperty* Property, const nlohmann::json& Value)
    {
        ValidateJsonValueType(Property, Value);
        const std::string resourcePrefix = "$resource/";

        auto ParsedValue = Value.get<std::string>();

        RC::StringType PackagePath = RC::to_generic_string(ParsedValue);

        if (ParsedValue.starts_with(resourcePrefix))
        {
            // Before: "$resource/modname/resourcename"
            // After:  "modname/resourcename"
            PackagePath = PackagePath.erase(0, resourcePrefix.length());

            // "/Engine/Transient.PalSchema/Resources/modname/resourcename"
            PackagePath = PS::Format("/Engine/Transient.PalSchema/Resources/{}", PackagePath);
        }

        auto SoftObjectPtr = UECustom::TSoftObjectPtr<UObject>(UECustom::FSoftObjectPath(PackagePath));
        FMemory::Memcpy(Data, &SoftObjectPtr, sizeof(UECustom::TSoftObjectPtr<UObject>));
    }

    void PropertyHelper::SetStructPropertyValueFromJsonValue(void* Data, RC::Unreal::FStructProperty* Property, const nlohmann::json& Value)
    {
        ValidateJsonValueType(Property, Value);

        auto ParsedObject = Value.get<nlohmann::json>();

        auto Struct = Property->GetStruct();
        if (!Struct)
        {
            throw std::runtime_error(std::format("Failed to get Struct"));
        }

        FField* Field = Struct->GetChildProperties();
        while (Field)
        {
            auto FieldName = GetPropertyNameAsUTF8String(static_cast<FProperty*>(Field));
            if (Value.contains(FieldName))
            {
                CopyJsonValueToContainer(Data, static_cast<FProperty*>(Field), Value.at(FieldName));
            }

            Field = GetNextField(Field);
        }
    }

    void PropertyHelper::SetArrayPropertyValueFromJsonValue(void* Data, RC::Unreal::FArrayProperty* Property, const nlohmann::json& Value)
    {
        ValidateJsonValueType(Property, Value);

        auto ParsedValue = Value.get<nlohmann::json>();

        PS::Trace("    ARRAY: helper anlegen");
        auto ScriptArray = static_cast<FScriptArray*>(Data);
        auto ScriptArrayHelper = UECustom::FScriptArrayHelper(ScriptArray, Property);

        PS::Trace("    ARRAY: GetInner");
        auto InnerProperty = Property->GetInner();
        PS::Trace("    ARRAY: inner=%p", (void*)InnerProperty);

        // Messpunkt Werkbank-Rezept (2026-08-15): nach dem Schreiben zurücklesen, was
        // tatsächlich im Array steht, statt nur dem Erfolg der Schreiboperation zu
        // vertrauen (siehe Learning 6: "Eine Erfolgsmeldung ist kein Wirkungsnachweis").
        auto TraceReadback = [&]() {
            int ReadIndex = 0;
            ScriptArrayHelper.ForEachElement([&](void* ElemData) {
                char Label[48];
                std::snprintf(Label, sizeof(Label), "    ARRAY-READBACK[%d]", ReadIndex);
                PropertyHelper::TraceScalarValue(Label, InnerProperty, ElemData);
                ++ReadIndex;
            });
        };

        if (Value.is_object())
        {
            if (Value.contains("Action"))
            {
                auto Action = Value.at("Action").get<std::string>();
                if (Action == "Clear")
                {
                    ScriptArrayHelper.Empty();
                }
            }

            if (Value.contains("Items"))
            {
                if (!Value.at("Items").is_array())
                {
                    throw std::runtime_error(std::format("Field Items must be an array"));
                }

                auto Items = Value.at("Items").get<nlohmann::json::array_t>();
                int TraceIndex = 0;
                for (auto& Item : Items)
                {
                    PS::Trace("    ITEM %d: init", TraceIndex);
                    UECustom::FManagedValue ValuePtr;
                    ScriptArrayHelper.InitializeValue(ValuePtr);
                    PS::Trace("    ITEM %d: befuellen", TraceIndex);
                    CopyJsonValueToContainer(ValuePtr.GetData(), InnerProperty, Item);
                    PS::Trace("    ITEM %d: anhaengen", TraceIndex);
                    ScriptArrayHelper.Add(ValuePtr);
                    PS::Trace("    ITEM %d: fertig", TraceIndex);
                    ++TraceIndex;
                }

                TraceReadback();
            }
        }
        else if (Value.is_array())
        {
            ScriptArrayHelper.Empty();

            auto Items = Value.get<nlohmann::json::array_t>();
            for (auto& Item : Items)
            {
                UECustom::FManagedValue ValuePtr;
                ScriptArrayHelper.InitializeValue(ValuePtr);
                CopyJsonValueToContainer(ValuePtr.GetData(), InnerProperty, Item);
                ScriptArrayHelper.Add(ValuePtr);
            }

            TraceReadback();
        }
    }

    void PropertyHelper::SetMapPropertyValueFromJsonValue(void* Data, RC::Unreal::FMapProperty* Property, const nlohmann::json& Value)
    {
        ValidateJsonValueType(Property, Value);

        auto ArrayItems = Value.get<std::vector<nlohmann::json>>();

        auto KeyProperty = Property->GetKeyProp();
        auto ValueProperty = Property->GetValueProp();

        auto MapLayout = FScriptMap::GetScriptLayout(
            KeyProperty->GetSize(),
            KeyProperty->GetMinAlignment(),
            ValueProperty->GetSize(),
            ValueProperty->GetMinAlignment());

        auto ScriptMap = static_cast<Unreal::FScriptMap*>(Data);
        auto ScriptMapHelper = UECustom::FScriptMapHelper(ScriptMap, MapLayout, KeyProperty, ValueProperty);

        for (const auto& Entry : ArrayItems)
        {
            if (!Entry.contains("Key") || !Entry.contains("Value"))
            {
                throw std::runtime_error("Each TMap entry must have a 'Key' and 'Value' property.");
            }

            UECustom::FManagedValue ScopedPair;

            ScriptMapHelper.InitializePair(ScopedPair);

            CopyJsonValueToContainer(ScopedPair.GetData(), KeyProperty, Entry.at("Key"));
            CopyJsonValueToContainer(ScopedPair.GetData(), ValueProperty, Entry.at("Value"));

            ScriptMapHelper.Add(ScopedPair);
        }

        ScriptMap->Rehash(MapLayout,
        [&](const void* Src) -> uint32 {
            return KeyProperty->GetValueTypeHash(Src);
        });
    }

    void PropertyHelper::ValidateJsonValueType(RC::Unreal::FProperty* Property, const nlohmann::json& Value)
    {
        auto PropertyName = GetPropertyNameAsUTF8String(Property);
        auto PropertyClass = Property->GetClass();
        auto PropertyClassName = PropertyClass.GetName();

        if (auto EnumProperty = CastProperty<FEnumProperty>(Property))
        {
            if (!Value.is_string()) throw std::runtime_error(std::format("Property {} must be a string", PropertyName));
        }
        else if (auto NumProperty = CastProperty<FNumericProperty>(Property))
        {
            if (!Value.is_number()) throw std::runtime_error(std::format("Property {} must be a number", PropertyName));
        }
        else if (auto BoolProperty = CastProperty<FBoolProperty>(Property))
        {
            if (!Value.is_boolean()) throw std::runtime_error(std::format("Property {} must be a boolean", PropertyName));
        }
        else if (auto NameProperty = CastProperty<FNameProperty>(Property))
        {
            if (!Value.is_string()) throw std::runtime_error(std::format("Property {} must be a string", PropertyName));
        }
        else if (auto StrProperty = CastProperty<FStrProperty>(Property))
        {
            if (!Value.is_string()) throw std::runtime_error(std::format("Property {} must be a string", PropertyName));
        }
        else if (auto TextProperty = CastProperty<FTextProperty>(Property))
        {
            if (!Value.is_string()) throw std::runtime_error(std::format("Property {} must be a string", PropertyName));
        }
        else if (auto ClassProperty = CastProperty<FClassProperty>(Property))
        {
            if (!Value.is_string()) throw std::runtime_error(std::format("Property {} must be a string", PropertyName));
        }
        else if (auto ObjectProperty = CastProperty<FObjectProperty>(Property) && PropertyClassName == STR("ObjectProperty"))
        {
            if (!Value.is_object() && !Value.is_string()) throw std::runtime_error(std::format("Property {} must be an object or string", PropertyName));
        }
        else if (auto SoftObjectProperty = CastProperty<FSoftObjectProperty>(Property) && PropertyClassName == STR("SoftObjectProperty"))
        {
            if (!Value.is_string()) throw std::runtime_error(std::format("Property {} must be a string", PropertyName));
        }
        else if (auto SoftClassProperty = CastProperty<FSoftClassProperty>(Property) && PropertyClassName == STR("SoftClassProperty"))
        {
            if (!Value.is_string()) throw std::runtime_error(std::format("Property {} must be a string", PropertyName));
        }
        else if (auto StructProperty = CastProperty<FStructProperty>(Property))
        {
            if (!Value.is_object()) throw std::runtime_error(std::format("Property {} must be an object", PropertyName));
        }
        else if (auto ArrayProperty = CastProperty<FArrayProperty>(Property))
        {
            if (!Value.is_object() && !Value.is_array()) throw std::runtime_error(std::format("Property {} must be an object or array", PropertyName));
        }
        else if (auto MapProperty = CastProperty<FMapProperty>(Property))
        {
            if (!Value.is_array()) throw std::runtime_error(std::format("Property {} must be an array of objects", PropertyName));
        }
    }

    std::string PropertyHelper::GetPropertyNameAsUTF8String(FProperty* Property)
    {
        auto PropertyName = Property->GetName();
        auto PropertyNameUTF8 = RC::to_string(PropertyName);
        return PropertyNameUTF8;
    }

    std::string PropertyHelper::GetPropertyTypeAsUTF8String(FProperty* Property)
    {
#ifdef __linux__
        // GetCPPType() ist unter Linux doppelt unbrauchbar:
        //  1. UE4SS' vtable-Offset (0x68) ist um einen Slot zu klein (Itanium-ABI
        //     hat zwei Destruktor-Slots) und trifft PassCPPArgsByRef.
        //  2. Der zurueckgegebene FString liegt im Heap des Spiels, UE4SS'
        //     FMemory::Free ruft aber ::free() -> free(): invalid pointer.
        // Der FFieldClass-Name ist nicht-virtuell erreichbar und genuegt hier.
        return RC::to_string(Property->GetClass().GetName());
#else
        auto PropertyType = RC::to_string(*Property->GetCPPType());
        return PropertyType;
#endif
    }

    RC::Unreal::FProperty* PropertyHelper::GetPropertyByName(RC::Unreal::UClass* Class, const RC::StringType& PropertyName)
    {
        FProperty* Property = nullptr;
        for (FProperty* It = Class->GetPropertyLink(); It != nullptr; It = It->GetPropertyLinkNext())
        {
            if (It->GetName() == PropertyName)
            {
                Property = It;
            }
        }
        return Property;
    }

    RC::Unreal::FProperty* PropertyHelper::GetPropertyByName(RC::Unreal::UScriptStruct* Struct, const RC::StringType& PropertyName)
    {
        FProperty* Property = nullptr;
        FName PropertyFName = FName(PropertyName, FNAME_Add);
        for (FProperty* It = Struct->GetPropertyLink(); It != nullptr; It = It->GetPropertyLinkNext())
        {
            if (It->GetFName() == PropertyFName)
            {
                Property = It;
            }
        }
        return Property;
    }

    void* PropertyHelper::GetValuePtrByPropertyNameInChain(RC::Unreal::UObject* Instance, const RC::StringType& PropertyName)
    {
        if (!Instance)
        {
            return nullptr;
        }

        RC::Unreal::FProperty* Property = PropertyHelper::GetPropertyByName(Instance->GetClassPrivate(), PropertyName);
        if (!Property)
        {
            return nullptr;
        }

        auto ValuePtr = Property->ContainerPtrToValuePtr<void>(Instance);
        return ValuePtr;
    }

    RC::Unreal::FFieldClass* PropertyHelper::FindFieldClassByName(const RC::Unreal::FName& Name)
    {
        auto NameToFieldClassMap = GetNameToFieldClassMap();
        if (!NameToFieldClassMap)
        {
            return nullptr;
        }

        auto FieldClass = NameToFieldClassMap->Find(Name);
        if (!FieldClass)
        {
            return nullptr;
        }

        return *FieldClass;
    }

    FFieldClass* PropertyHelper::FindFieldClassByName(const RC::StringType& Name)
    {
        auto NewName = FName(Name, FNAME_Add);
        return FindFieldClassByName(NewName);
    }

    FField* PropertyHelper::GetNextField(FField* Field)
    {
        auto Next = *Helper::Casting::ptr_cast<FField**>(Field, 0x20);
        return Next;
    }

    TMap<FName, FFieldClass*>* PropertyHelper::GetNameToFieldClassMap()
    {
#ifdef __linux__
        // Linux: Die AOB-Signatur greift nicht. UE4SS stellt dieselbe Map als eigene
        // statische Methode bereit (Unreal/FField.hpp) — direkt nutzbar, ohne Scan.
        return &RC::Unreal::FFieldClass::GetNameToFieldClassMap();
#else
        using GetNameToFieldClassMap_Signature = TMap<FName, FFieldClass*>*(*)();
        static GetNameToFieldClassMap_Signature GetNameToFieldClassMap_Internal = nullptr;

        if (!GetNameToFieldClassMap_Internal)
        {
            GetNameToFieldClassMap_Internal = reinterpret_cast<GetNameToFieldClassMap_Signature>(
                Palworld::SignatureManager::GetSignature("FFieldClass::GetNameToFieldClassMap")
            );
        }

        if (!GetNameToFieldClassMap_Internal)
        {
            PS::Log<LogLevel::Error>(STR("Failed to call FFieldClass::GetNameToFieldClassMap because function address was invalid."));
            return nullptr;
        }

        return GetNameToFieldClassMap_Internal();
#endif
    }

    bool PropertyHelper::IsPropertyA(RC::Unreal::FField* Field, RC::Unreal::FFieldClass* FieldClass)
    {
#ifdef __linux__
        // Linux: AOB-Signatur greift nicht. UE4SS bietet FField::IsA(const FFieldClass*)
        // als eigene Methode an (Unreal/FField.hpp) — direkt nutzbar.
        if (!Field || !FieldClass) return false;
        return Field->IsA(FieldClass);
#else
        using IsA_Signature = bool(*)(FField*, FFieldClass*);
        static IsA_Signature IsA_Internal = nullptr;

        if (!IsA_Internal)
        {
            IsA_Internal = reinterpret_cast<IsA_Signature>(
                Palworld::SignatureManager::GetSignature("FField::IsA")
            );
        }

        if (!IsA_Internal)
        {
            PS::Log<LogLevel::Error>(STR("Failed to call FField::IsA because function address was invalid.\n"));
            return false;
        }

        return IsA_Internal(Field, FieldClass);
#endif
    }
}