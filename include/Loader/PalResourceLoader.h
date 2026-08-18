#pragma once

#include "Loader/PalModLoaderBase.h"
#include <filesystem>
#include <unordered_map>

namespace RC::Unreal {
    class UObject;
}

namespace Palworld {
    class PalResourceLoader : public PalModLoaderBase {
    public:
        PalResourceLoader();
        ~PalResourceLoader();
    protected:
        virtual void OnLoad(const std::filesystem::path& loaderPath, const RC::StringType& modName, const EEngineLifecyclePhase& engineLifecyclePhase) override final;
        virtual void OnAutoReload(const RC::StringType& modName, const std::filesystem::path& modFilePath) override final;

        virtual bool CanInitialize(const EEngineLifecyclePhase& engineLifecyclePhase) override final;
        virtual bool OnInitialize() override final;
    private:
        void RegisterResourceAsset(const RC::StringType& modName, RC::Unreal::UObject* resource);
        void UnregisterResourceAsset(RC::Unreal::UObject* resource);
        void UnregisterResourceAssetByFilePath(const RC::StringType& modName, const std::filesystem::path& modFilePath);
        void UnregisterResourceAssets(const RC::StringType& modName);
        void UnregisterResourceAssets();

        void LoadImages(const RC::StringType& modName, const std::filesystem::path& resourcesPath);
        void LoadImage(const RC::StringType& modName, const std::filesystem::path& imagePath);

        std::unordered_map<RC::StringType, std::vector<RC::Unreal::UObject*>> m_loadedResourcesMap;
    };
}