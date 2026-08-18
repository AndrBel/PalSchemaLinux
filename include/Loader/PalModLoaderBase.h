#pragma once

#include "Unreal/NameTypes.hpp"
#include "SDK/Classes/Custom/UDataTableStore.h"
#include "nlohmann/json.hpp"
#include <string>

namespace RC::Unreal {
	class UDataTable;
}

namespace Palworld {
    enum class EEngineLifecyclePhase {
        PreEngineInit,
        PostEngineInit,
        UE4SSInit,
        GameInstanceInit,
    };

	class PalModLoaderBase {
	public:
		virtual ~PalModLoaderBase();
        
        void AssignDatatableRegistry(UECustom::UDataTableRegistry& datatableRegistry);

        const RC::StringType& GetDisplayName() const;

        void Setup();
        void AutoReload(const RC::StringType& modName, const std::filesystem::path& modFilePath);
        void Load(const std::filesystem::path& modPath, const RC::StringType& modName, const EEngineLifecyclePhase& engineLifecyclePhase);

		void Initialize(const EEngineLifecyclePhase& engineLifecyclePhase);

        const bool& HasInitialized() const;

        // Returns the folder name specified for this loader, e.g. blueprints, raw, pals, etc.
        const std::string& GetModFolderType();

#ifdef __linux__
        // Linux: UE4SS startet erst, wenn die Engine ihre Objekte laengst geladen hat.
        // Hooks auf Ladeereignisse (UDataTable::Serialize, UBlueprintGeneratedClass::PostLoad)
        // feuern fuer diese Objekte nie mehr. Loader koennen ihre Aenderungen hier
        // nachtraeglich auf den bereits vorhandenen Objektbestand anwenden.
        // Wird von PalMainLoader::Initialize() aufgerufen, NACHDEM die Mods geladen sind.
        virtual void ApplyToExistingObjects() {}
#endif
    protected:
        PalModLoaderBase(const std::string& modFolderName);

        void SetDisplayName(const RC::StringType& displayName);

        void IterateModsFolder(const std::function<void(const std::filesystem::path&, const RC::StringType&)>& callback);

        // Does not throw if the data table isn't found, returns nullptr
        RC::Unreal::UDataTable* TryGetDatatableByName(const std::string& name);

        // Linux-Portierung: WIRFT NICHT MEHR. Im Serverprozess liegen drei
        // C++-Laufzeiten uebereinander (PalServer = statisches libc++/libc++abi,
        // libsteam_api.so = statisches libstdc++, PalSchema = libstdc++.so.6).
        // 'throw' bindet an libc++abis __cxa_throw, die Landing Pads an
        // __gxx_personality_v0 aus libsteam_api -- die Exception gilt dort als
        // __foreign_exception und der Prozess stirbt mit SIGSEGV. Nachgewiesen an
        // GetDatatableByName("DT_PalMonsterParameter").
        // Deshalb: fehlende Tabelle protokollieren, nullptr liefern und ein Flag
        // setzen, das der Aufrufer mit HasDatatableLookupFailed() abfragt.
        RC::Unreal::UDataTable* GetDatatableByName(const std::string& name);

        // true, wenn seit dem Start von OnInitialize() eine Tabelle gefehlt hat.
        bool HasDatatableLookupFailed() const;
    protected:
        // Called when the Loader is created, calling UE functions during Setup is not safe
        virtual void OnSetup();
        virtual void OnLoad(const std::filesystem::path& loaderPath, const RC::StringType& modName, const EEngineLifecyclePhase& engineLifecyclePhase);
        virtual void OnAutoReload(const RC::StringType& modName, const std::filesystem::path& modFilePath);

        virtual bool CanInitialize(const EEngineLifecyclePhase& engineLifecyclePhase) = 0;

        // You should return true or false depending on if the initialization was successful
        virtual bool OnInitialize() = 0;

        // Do post initialize logic here like loading mods, etc.
        virtual void PostInitialize();

        virtual void OnDatatableSerialized(RC::Unreal::UDataTable* datatable);
    private:
        void Initialize_Internal();

        std::string m_modFolderType = "";
        RC::StringType m_displayName = TEXT("Unknown Loader");
        UECustom::UDataTableRegistry* m_datatableRegistry = nullptr;
        bool m_hasInitialized = false;
        bool m_datatableLookupFailed = false;
        std::mutex m_mutex;

        UECustom::DatatableSerializeCallbackId m_datatableSerializeCallbackId{};
	};
}