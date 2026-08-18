#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include "Utility/LinuxVTable.h"
#include <vector>
#include <unordered_map>
#include "Utility/LinuxCompat.h"
#include "Utility/LinuxFormat.h"
#include <fstream>
#include <filesystem>
#include "Unreal/CoreUObject/UObject/Class.hpp"
#include "Unreal/UFunction.hpp"
#include "Unreal/Hooks.hpp"
#include "Utility/Config.h"
#include "Utility/Logging.h"
#include "SDK/Classes/Async.h"
#include "SDK/Classes/Custom/UDataTableStore.h"
#include "SDK/Classes/Custom/UObjectGlobals.h"
#include "SDK/Classes/UCompositeDataTable.h"
#include "SDK/Classes/UWorldPartitionRuntimeLevelStreamingCell.h"
#include "SDK/Classes/PalUtility.h"
#include "SDK/PalSignatures.h"
#include "SDK/StaticClassStorage.h"
#include "SDK/UnrealOffsets.h"
#include "UE4SSProgram.hpp"
#include "Loader/PalMonsterModLoader.h"
#include "Loader/PalHumanModLoader.h"
#include "Loader/PalLanguageModLoader.h"
#include "Loader/PalItemModLoader.h"
#include "Loader/PalSkinModLoader.h"
#include "Loader/PalAppearanceModLoader.h"
#include "Loader/PalBuildingModLoader.h"
#include "Loader/PalRawTableLoader.h"
#include "Loader/PalBlueprintModLoader.h"
#include "Loader/PalEnumLoader.h"
#include "Loader/PalHelpGuideModLoader.h"
#include "Loader/PalSpawnLoader.h"
#include "Loader/PalMainLoader.h"
#include "Misc/FileWatchWrapper.h"

using namespace RC;
using namespace RC::Unreal;

namespace fs = std::filesystem;

namespace {
    // ---------------------------------------------------------------------
    // Linux-Portierung: vtable-Slot von UPalGameInstance::Init
    //
    // PalSchema greift den Slot ueber PGIVTablePtr[N] ab, also gezaehlt AB dem
    // vptr einer Instanz (OHNE die zwei _ZTV-Kopfeintraege). Der Windows-Wert
    // 90 ist unter dem Itanium-C++-ABI falsch.
    //
    // Verifiziert an PalServer-Linux-Shipping v1.0.3.101283 (EXEC/non-PIE):
    //   _ZTV7UObject           = 712 Byte  => 87 Funktionszeiger (0..86)
    //   _ZTV13UGameInstance    = 1128 Byte => primaer 0..133, dann Sekundaer-vtable
    //   _ZTV16UPalGameInstance = 1168 Byte; ueberschreibt gegenueber
    //     UGameInstance genau die Funktionszeiger 92, 93, 96, 104, 105, 106
    //     (134/135 sind neu hinzugekommene Pal-Virtuals).
    //
    //   UGameInstance-Slot 92 = 0xa3bbde0 beginnt mit
    //       mov 0x1ebe9fa(%rip),%rsi   # c27a7f0  (globaler statischer FName)
    //       call 0x7b575f0             # FindFunctionChecked
    //       mov (%r14),%rcx ; call *0x268(%rcx)   # UObject::ProcessEvent
    //     Der FName bei 0xc27a7f0 wird im Static-Init bei 0xa3ba026 aus dem
    //     UTF-16-Literal 0xae3966 = "ReceiveInit" konstruiert.
    //     => Slot 92 ist UGameInstance::Init(); erste Anweisung ist ReceiveInit().
    //   Slot 93 = 0xa3bc250 laedt analog 0xc27a7f8 = "ReceiveShutdown"
    //     => Shutdown(), der direkte Nachbar. Passt zur Deklarationsreihenfolge
    //     in GameInstance.h.
    //   UPalGameInstance-Slot 92 = 0x71f98f0 ruft 0xa3bbde0 auf (Super::Init).
    //
    //   Gegenprobe: die frueher benutzten Slots 88-90 sind Thunks
    //   (mov 0x20(%rdi),%rdi ; jmp *0x4xx(%rax)), 91 nimmt vier Argumente.
    // ---------------------------------------------------------------------
#ifdef __linux__
    constexpr size_t PS_VT_SLOT_GAMEINSTANCE_INIT = 92;   // MSVC: 90
#else
    constexpr size_t PS_VT_SLOT_GAMEINSTANCE_INIT = 90;
#endif

    // Wie in PalBlueprintModLoader.cpp: niemals auf einen geteilten Trivial-Stub
    // hooken. Sitzt der Slot daneben, ist die Default-Implementierung oft ein
    // winziger, von allen Klassen benutzter Stub; ein Detour darauf zerlegt die
    // halbe Engine (und faelscht bei bool-Virtuals den Rueckgabewert).
    // Sucht ein lebendes (Nicht-CDO) Objekt, dessen Klasse von TargetClass erbt.
    // Der Klassen-Check wird pro UClass gemerkt, damit die Super-Kette nicht fuer
    // jedes der hunderttausenden UObjects erneut abgelaufen wird.
    RC::Unreal::UObject* FindLiveInstanceOf(RC::Unreal::UClass* TargetClass)
    {
        if (!TargetClass) return nullptr;

        std::unordered_map<const void*, bool> IsDerivedCache;
        RC::Unreal::UObject* Result = nullptr;

        RC::Unreal::UObjectGlobals::ForEachUObject(
            [&](RC::Unreal::UObject* Object, RC::Unreal::int32, RC::Unreal::int32) {
                if (!Object) return RC::LoopAction::Continue;
                auto* Cls = Object->GetClassPrivate();
                if (!Cls) return RC::LoopAction::Continue;

                auto It = IsDerivedCache.find(Cls);
                bool IsDerived;
                if (It != IsDerivedCache.end())
                {
                    IsDerived = It->second;
                }
                else
                {
                    IsDerived = false;
                    for (RC::Unreal::UStruct* S = Cls; S; S = S->GetSuperStruct())
                    {
                        if (S == static_cast<RC::Unreal::UStruct*>(TargetClass)) { IsDerived = true; break; }
                    }
                    IsDerivedCache.emplace(Cls, IsDerived);
                }

                if (!IsDerived) return RC::LoopAction::Continue;
                if (Object->HasAnyFlags(RC::Unreal::RF_ClassDefaultObject)) return RC::LoopAction::Continue;

                Result = Object;
                return RC::LoopAction::Break;
            });

        return Result;
    }

    bool PsIsTrivialStubMain(const void* fn)
    {
        if (!fn) return true;
        const auto* b = static_cast<const unsigned char*>(fn);
        if (b[0] == 0xC3) return true;
        if (b[0] == 0x31 && b[1] == 0xC0 && b[2] == 0xC3) return true;
        if (b[0] == 0x33 && b[1] == 0xC0 && b[2] == 0xC3) return true;
        if (b[0] == 0xB0 && b[2] == 0xC3) return true;
        return false;
    }
}

namespace Palworld {
    PalMainLoader::PalMainLoader() {
        CreateLoaders();
    }

    PalMainLoader::~PalMainLoader()
    {
        auto expected1 = DatatableSerialize_Hook.disable();
        DatatableSerialize_Hook = {};

        auto expected2 = GameInstanceInit_Hook.disable();
        GameInstanceInit_Hook = {};

        auto expected4 = GetPakFolders_Hook.disable();
        GetPakFolders_Hook = {};

        DatatableSerializeCallbacks.clear();
        GameInstanceInitCallbacks.clear();
        GetPakFoldersCallback.clear();
    }

    void PalMainLoader::PreInitialize()
    {
        HookDatatableSerialize();
        SetupAlternativePakPathReader();
    }

    void PalMainLoader::Initialize()
	{
        SetupAutoReload();
#ifdef __linux__
        // Ladeadresse von main.so protokollieren. Der Signal-Handler meldet nur ein absolutes
        // rip; erst mit der Basis laesst sich daraus ein Offset in der Bibliothek rechnen und
        // die Absturzstelle disassemblieren. Ohne das bleibt jede Zuordnung Raterei.
        {
            Dl_info Info{};
            if (::dladdr(reinterpret_cast<void*>(&PalMainLoader::Initialize), &Info) && Info.dli_fbase)
            {
                PS::Log<LogLevel::Normal>(STR("main.so geladen bei {}"), Info.dli_fbase);
            }
            // Vollstaendige Speicherkarte sichern: der Signal-Handler meldet ein absolutes rip,
            // erst damit laesst sich sagen, in WELCHEM Modul die Absturzstelle liegt.
            if (const char* TracePath = ::getenv("PS_TRACE_FILE"))
            {
                (void)TracePath;
                if (std::FILE* Src = std::fopen("/proc/self/maps", "r"))
                {
                    if (std::FILE* Dst = std::fopen("/home/pwtest/ps-maps.txt", "w"))
                    {
                        char Buffer[4096];
                        while (std::fgets(Buffer, sizeof(Buffer), Src)) { std::fputs(Buffer, Dst); }
                        std::fclose(Dst);
                    }
                    std::fclose(Src);
                }
            }
        }
        ScanExistingDataTables();
        // Loader, deren Ladehooks unter Linux zu spaet kommen, wenden ihre Aenderungen
        // hier auf den bereits vorhandenen Objektbestand an. Der Zeitpunkt ist bewusst
        // gewaehlt: die Mod-Dateien sind eingelesen, die Engine-Objekte stehen.
        for (auto& loader : m_loaders)
        {
            if (loader) loader->ApplyToExistingObjects();
        }
#endif
	}

#ifdef __linux__
    void PalMainLoader::ScanExistingDataTables()
    {
        if (getenv("PS_SKIP_DT_SCAN")) { PS::Log<LogLevel::Normal>(STR("ScanExistingDataTables skipped.")); return; }
        // StaticFindObject("/Script/Engine.DataTable") liefert hier nichts, deshalb
        // die Klasse waehrend der Iteration selbst bestimmen: ein UObject, dessen
        // Klasse "DataTable" heisst, gibt uns die UClass — danach per IsA filtern.
        // Vergleich ueber FName statt String: FName ist intern ein Index-Paar,
        // der Vergleich ist damit ein Integer-Vergleich ohne jede Allokation.
        // Die vorherige Variante konvertierte fuer JEDES Objekt im GUObjectArray
        // einen String (RC::to_string) — hunderttausende Allokationen quer ueber
        // Bibliotheksgrenzen, was zu 'free(): invalid pointer' fuehrte.
        static const RC::Unreal::FName DataTableFName(STR("DataTable"));
        // Composite-Tabellen (UCompositeDataTable) kopieren ihre Zeilen beim Laden aus den
        // Elterntabellen (_Common) in einen eigenen RowMap-Cache. Das Spiel liest zur Laufzeit
        // die Composite -- ein Patch auf _Common bleibt deshalb wirkungslos (Symptom: Log meldet
        // "488 rows updated", im Spiel faellt nichts). Da UCompositeDataTable von UDataTable erbt,
        // laesst sie sich direkt wie eine normale Tabelle registrieren und patchen. Die
        // Eltern-Zuordnung (GetParentTables) wird dabei bewusst NICHT angefasst -- genau dort lag
        // der fruehere "mremap_chunk(): invalid pointer".
        static const RC::Unreal::FName CompositeDataTableFName(STR("CompositeDataTable"));
        const bool bIncludeComposite = getenv("PS_COMPOSITE_TABLES") != nullptr;
        int32_t CompositeCount = 0;
        // OFFEN (Versuch zurueckgenommen): Klassen namens "CompositeDataTable"
        // zusaetzlich einzusammeln. Sie tragen die Eltern-Zuordnung, ueber die
        // GetDatatableByName("DT_PalMonsterParameter") ueberhaupt erst aufloesbar
        // waere -- ohne sie koennen die GameInstanceInit-Loader keine Tabelle finden.
        // Der Scan ueber alle ~350 Composite-Tabellen endet aber reproduzierbar in
        // "mremap_chunk(): invalid pointer" (SIGABRT), auch nachdem
        // UCompositeDataTable::GetParentTables() auf einen Zeiger (statt TArray-Kopie)
        // umgestellt wurde. Der verbleibende Verdacht liegt auf den Allokationen in
        // UDataTableRegistry::Add / PalRawTableLoader::Apply, die ueber die
        // Modulgrenze PalSchema <-> UE4SS laufen. Muss separat untersucht werden.
        RC::Unreal::UClass* DataTableClass = nullptr;
        std::vector<RC::Unreal::UDataTable*> Found;

        RC::Unreal::UObjectGlobals::ForEachUObject(
            [&](RC::Unreal::UObject* Object, RC::Unreal::int32, RC::Unreal::int32) {
                if (!Object) return RC::LoopAction::Continue;
                auto* Cls = Object->GetClassPrivate();
                if (!Cls) return RC::LoopAction::Continue;
                const RC::Unreal::FName ClsName = Cls->GetNamePrivate();
                if (ClsName == DataTableFName)
                {
                    if (!DataTableClass) DataTableClass = Cls;
                    Found.push_back(static_cast<RC::Unreal::UDataTable*>(Object));
                }
                else if (bIncludeComposite && ClsName == CompositeDataTableFName)
                {
                    ++CompositeCount;
                    Found.push_back(static_cast<RC::Unreal::UDataTable*>(Object));
                }
                return RC::LoopAction::Continue;
            });

        if (Found.empty())
        {
            PS::Log<LogLevel::Error>(STR("ScanExistingDataTables: no DataTable objects found."));
            return;
        }

        if (getenv("PS_SCAN_COUNT_ONLY"))
        {
            PS::Log<LogLevel::Normal>(STR("ScanExistingDataTables: count-only mode."));
        }
        else
        {
            // Fortschritt ungepuffert mitschreiben: stuerzt die Registrierung ab,
            // steht in der Datei, welche Tabelle zuletzt dran war.
            // Fortschrittsprotokoll nur bei Bedarf (PS_SCAN_PROGRESS=<Pfad>) — hilft,
            // eine abstuerzende Tabelle zu identifizieren.
            const char* progressPath = getenv("PS_SCAN_PROGRESS");
            FILE* progress = progressPath ? fopen(progressPath, "w") : nullptr;
            size_t index = 0;
            for (auto* Table : Found)
            {
                if (progress)
                {
                    auto NameNarrow = RC::to_string(Table->GetNamePrivate().ToString());
                    fputs(NameNarrow.c_str(), progress);
                    fputc(10, progress);
                    fflush(progress);
                }
                for (auto& Callback : DatatableSerializeCallbacks)
                {
                    Callback(Table);
                }
                ++index;
            }
            if (progress)
            {
                fputs("DONE", progress);
                fputc(10, progress);
                fclose(progress);
            }
        }
        PS::Log<LogLevel::Normal>(STR("ScanExistingDataTables: registered {} tables ({} davon Composite)."), static_cast<int32_t>(Found.size()), CompositeCount);
    }
#endif

    void PalMainLoader::AutoReload(const std::filesystem::path& filePath)
    {
        // Skip to the PalSchema folder and start our iterator from there
        auto it = std::find_if(filePath.begin(), filePath.end(),
            [](const auto& p) { return p == "PalSchema"; });

        if (it == filePath.end() || std::distance(it, filePath.end()) < 4)
        {
            return;
        }

        // Skip PalSchema and mods folder
        std::advance(it, 2);
        auto modName = it->native();

        // Move to folder type, e.g. buildings
        std::advance(it, 1);
        auto folderType = it->string();

        std::ifstream f(filePath);
        if (f.peek() == std::ifstream::traits_type::eof()) {
            return;
        }
        f.close();

        UECustom::AsyncTask(UECustom::ENamedThreads::GameThread, [this, filePath, folderType, modName]() {
            try
            {
                for (auto& loader : m_loaders)
                {
                    if (loader->GetModFolderType() == folderType)
                    {
                        loader->AutoReload(RC::to_wstring(modName), filePath);
                        PS::Log<LogLevel::Normal>(STR("Auto-reloaded mod {}\n"), RC::to_wstring(modName));
                        break;
                    }
                }
            }
            catch (const std::exception& e)
            {
                PS::Log<LogLevel::Error>(STR("Failed to auto-reload mod {} - {}\n"), RC::to_wstring(modName), RC::to_generic_string(e.what()));
            }
        });
    }

    void PalMainLoader::IterateModsFolder(const std::function<void(const std::filesystem::path&, const RC::StringType&)>& callback)
    {
        static auto modsPath = fs::path(UE4SSProgram::get_program().get_working_directory()) / "Mods" / "PalSchema" / "mods";
        if (fs::exists(modsPath))
        {
            for (const auto& entry : fs::directory_iterator(modsPath)) {
                if (entry.is_directory())
                {
                    auto& path = entry.path();
                    auto folderName = RC::to_wstring(path.stem().string()); // Linux: native() ist std::string
                    callback(entry.path(), folderName);
                }
            }
        }
    }

    void PalMainLoader::SetupPostEngineInitLoaders()
    {
        InitializeMods(EEngineLifecyclePhase::PostEngineInit);
        LoadMods(EEngineLifecyclePhase::PostEngineInit);
    }

    void PalMainLoader::RunGameInstanceInitLoadersOnce()
    {
        if (m_gameInstanceLoadersRan) return;
        m_gameInstanceLoadersRan = true;

#ifdef __linux__
        // OPT-IN, weil die Phase auf diesem Port noch nicht vollstaendig durchlaeuft.
        // Stand (PS_GAMEINSTANCE_LOADERS=1, Palworld v1.0.3.101283):
        //   initialisieren sauber : resources, appearance, helpguide, translations
        //   deaktivieren sich sauber: pals, npcs, items, skins, buildings
        //                             (Parent-DataTables nicht in der Registry, weil
        //                              ScanExistingDataTables die Composite-Tabellen
        //                              nicht einsammeln kann -- siehe dort)
        //                           spawns (Signatur UWorld::CleanupWorld fehlt)
        //   danach LoadMods(GameInstanceInit): "Applied changes to
        //   BP_BuildObject_AncientWorkBench_C", dann SIGSEGV -- der Blueprint-Pfad
        //   wirft eine Exception, und ein 'throw' ist in diesem Prozess toedlich
        //   (drei kollidierende C++-Laufzeiten, siehe CMakeLists.txt und
        //   PalModLoaderBase.h). Solange das nicht geloest ist, bleibt die Phase aus.
        if (!getenv("PS_GAMEINSTANCE_LOADERS"))
        {
            PS::Log<LogLevel::Normal>(STR("GameInstanceInit loaders are opt-in on Linux (set PS_GAMEINSTANCE_LOADERS=1)."));
            return;
        }
#endif

        try
        {
            SetupGameInstanceInitLoaders();
        }
        catch (const std::exception& e)
        {
            PS::Log<LogLevel::Error>(STR("GameInstanceInit loaders threw: {}"), RC::to_generic_string(e.what()));
        }
    }

    void PalMainLoader::SetupGameInstanceInitLoaders()
    {
        InitializeMods(EEngineLifecyclePhase::GameInstanceInit);
        LoadMods(EEngineLifecyclePhase::GameInstanceInit);
    }

    void PalMainLoader::HookDatatableSerialize()
    {
        auto DatatableSerializeFuncPtr = Palworld::SignatureManager::GetSignature("UDataTable::Serialize");
#ifdef __linux__
        // Linux: Die AOB-Signaturen stammen aus der Windows-Binary und koennen hier
        // prinzipiell nicht greifen (anderer Compiler => anderer Maschinencode).
        // PalServer-Linux-Shipping ist jedoch EXEC (nicht PIE) und exportiert
        // ~30.000 vtable-Symbole in .dynsym — virtuelle Methoden sind daher ueber
        // dlsym + Slot-Index direkt erreichbar, ganz ohne Byte-Muster.
        //
        // Slot-Herleitung: UDataTable ueberschreibt gegenueber UObject die Slots
        // {27,28,29,36,39,44,54}. Im Abgleich mit UStruct/UClass/UFunction/UPackage/
        // UScriptStruct/UEnum bleiben als von ALLEN ueberschrieben nur {28,36} —
        // die beiden Serialize-Ueberladungen. PalSchema hookt die FArchive&-Variante
        // (vgl. OnDataTableSerialized), in UObject zuerst deklariert => Slot 28.
        if (!DatatableSerializeFuncPtr)
        {
            DatatableSerializeFuncPtr = PS::LinuxVTable::GetVirtualFunction(
                "_ZTV10UDataTable", PS_DATATABLE_SERIALIZE_SLOT);
            if (DatatableSerializeFuncPtr)
            {
                PS::Log<LogLevel::Normal>(STR("UDataTable::Serialize via vtable slot resolved."));
            }
        }
#endif
        if (!DatatableSerializeFuncPtr)
        {
            PS::Log<LogLevel::Error>(STR("Unable to initialize PalSchema core, signature for UDataTable::Serialize is outdated.\n"));
            return;
        }

        DatatableSerialize_Hook = safetyhook::create_inline(reinterpret_cast<void*>(DatatableSerializeFuncPtr),
            OnDataTableSerialized);

        DatatableSerializeCallbacks.push_back([&](RC::Unreal::UDataTable* datatable) {
            InitCore();
            m_datatableRegistry.Add(datatable);
        });

        PS::Log<LogLevel::Verbose>(STR("Core pre-initialized.\n"));
    }

    void PalMainLoader::HookGameInstanceInit()
    {
        auto PalGameInstanceClass = UECustom::UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Pal.PalGameInstance"));
        if (!PalGameInstanceClass)
        {
            PS::Log<LogLevel::Error>(STR("Failed to find PalGameInstance. Cannot hook OnGameInstanceInit.\n"));
            return;
        }

        PS::Log<LogLevel::Verbose>(STR("Fetching default object for UPalGameInstance...\n"));
        uintptr_t** PGIVTablePtr = *(uintptr_t***)PalGameInstanceClass->GetClassDefaultObject();
        void* GameInstanceInitPtr = (void*)PGIVTablePtr[PS_VT_SLOT_GAMEINSTANCE_INIT];
        if (PsIsTrivialStubMain(GameInstanceInitPtr))
        {
            PS::Log<LogLevel::Error>(STR("Refusing to hook UPalGameInstance::Init at {}: target is a shared trivial stub, so the vtable slot is wrong."), GameInstanceInitPtr);
            return;
        }
        PS::Log<LogLevel::Normal>(STR("Found UPalGameInstance::Init: {} (vtable slot {})."), GameInstanceInitPtr, static_cast<int>(PS_VT_SLOT_GAMEINSTANCE_INIT));
        PS::Log<LogLevel::Verbose>(STR("Found UPalGameInstance::Init: {}\n"), GameInstanceInitPtr);

        GameInstanceInitCallbacks.push_back([&](UObject* Instance) {
            SetupGameInstanceInitLoaders();
        });

        GameInstanceInit_Hook = safetyhook::create_inline(GameInstanceInitPtr,
            reinterpret_cast<void*>(OnGameInstanceInit));

#ifdef __linux__
        // Linux: UE4SS' C++-Mods starten erst nach UnrealInit. UPalGameInstance::Init
        // ist zu diesem Zeitpunkt bereits gelaufen -- der eben gesetzte Hook feuert
        // in diesem Prozessleben also nicht mehr. Nur diagnostisch protokollieren.
        UObject* LiveGameInstance = FindLiveInstanceOf(PalGameInstanceClass);
        if (LiveGameInstance)
        {
            // Die Loader hier NICHT direkt starten: wir laufen auf UE4SS' Init-Thread,
            // waehrend der Game Thread schon arbeitet. Ein Versuch damit endete
            // reproduzierbar im SIGSEGV im 'pals'-Loader (LoadAsset_Blocking). Die
            // GameInstanceInit-Phase haengt stattdessen an HookInitGameState().
            PS::Log<LogLevel::Normal>(STR("UPalGameInstance::Init already ran before PalSchema started; GameInstanceInit loaders run from AGameModeBase::InitGameState instead."));
        }
#endif
    }

    void PalMainLoader::HookInitGameState()
    {
        auto InitGameStatePtr = Palworld::SignatureManager::GetSignature("AGameModeBase::InitGameState");
        if (!InitGameStatePtr)
        {
            PS::Log<LogLevel::Error>(STR("Unable to hook AGameModeBase::InitGameState, address is unknown."));
            return;
        }

        if (PsIsTrivialStubMain(InitGameStatePtr))
        {
            PS::Log<LogLevel::Error>(STR("Refusing to hook AGameModeBase::InitGameState at {}: target is a shared trivial stub."), InitGameStatePtr);
            return;
        }

        InitGameStateCallbacks.push_back([&](UObject* GameMode) {
            // InitGameState feuert bei jedem Map-Load erneut; die
            // GameInstanceInit-Phase soll genau einmal laufen (wie unter Windows
            // in UPalGameInstance::Init).
            if (m_gameInstanceLoadersRan) return;
            PS::Log<LogLevel::Normal>(STR("AGameModeBase::InitGameState reached, running GameInstanceInit loaders."));
            RunGameInstanceInitLoadersOnce();
        });

        InitGameState_Hook = safetyhook::create_inline(InitGameStatePtr,
            reinterpret_cast<void*>(OnInitGameState));

        PS::Log<LogLevel::Normal>(STR("Hooked AGameModeBase::InitGameState at {}."), InitGameStatePtr);

#ifdef __linux__
        // Zweite Leine fuer den Linux-Startfall: UE4SS startet seine C++-Mods erst,
        // wenn FEngineLoop::Init inklusive Startup-Map-Load durch ist. Dann sind
        // UPalGameInstance::Init UND AGameModeBase::InitGameState laengst gelaufen --
        // beide Hooks feuern in diesem Prozessleben nicht mehr und die Loader
        // items/pals/npcs/skins/buildings/spawns blieben tot.
        //
        // UEngine::Tick ist der erste wiederkehrende Einstiegspunkt danach und laeuft
        // auf dem GAME THREAD. Das ist wesentlich: ein Versuch, die Loader direkt vom
        // UE4SS-Init-Thread aus zu starten, endete reproduzierbar im SIGSEGV im
        // 'pals'-Loader (UKismetSystemLibrary::LoadAsset_Blocking).
        //
        // Der Tick-Hook ist in diesem UE4SS-Port aktiv (Log: "GameEngine::Tick
        // address (vtable: 0xaa45870; scan: 0xaa45870)"), UE4SS nutzt ihn selbst als
        // GameThreadInitializer. Nach dem ersten Durchlauf ist der Callback ein
        // reiner bool-Test.
        RC::Unreal::Hook::RegisterEngineTickPostCallback([this](RC::Unreal::UEngine*, float) {
            if (m_gameInstanceLoadersRan) return;
            PS::Log<LogLevel::Normal>(STR("First UEngine::Tick after PalSchema init; running GameInstanceInit loaders on the game thread."));
            RunGameInstanceInitLoadersOnce();
        });
#endif
    }

    void PalMainLoader::CreateLoaders()
    {
        auto resourceLoader = std::make_unique<PalResourceLoader>();
        RegisterLoader(std::move(resourceLoader));

        auto enumLoader = std::make_unique<PalEnumLoader>();
        RegisterLoader(std::move(enumLoader));

        auto monsterModLoader = std::make_unique<PalMonsterModLoader>();
        RegisterLoader(std::move(monsterModLoader));

        auto humanModLoader = std::make_unique<PalHumanModLoader>();
        RegisterLoader(std::move(humanModLoader));

        auto itemModLoader = std::make_unique<PalItemModLoader>();
        RegisterLoader(std::move(itemModLoader));

        auto skinModLoader = std::make_unique<PalSkinModLoader>();
        RegisterLoader(std::move(skinModLoader));

        auto appearanceModLoader = std::make_unique<PalAppearanceModLoader>();
        RegisterLoader(std::move(appearanceModLoader));

        auto buildingModLoader = std::make_unique<PalBuildingModLoader>();
        RegisterLoader(std::move(buildingModLoader));

        auto rawTableModLoader = std::make_unique<PalRawTableLoader>();
        RegisterLoader(std::move(rawTableModLoader));

        auto blueprintModLoader = std::make_unique<PalBlueprintModLoader>();
        RegisterLoader(std::move(blueprintModLoader));

        auto helpGuideLoader = std::make_unique<PalHelpGuideModLoader>();
        RegisterLoader(std::move(helpGuideLoader));

        auto spawnLoader = std::make_unique<PalSpawnLoader>();
        RegisterLoader(std::move(spawnLoader));

        auto languageModLoader = std::make_unique<PalLanguageModLoader>();
        RegisterLoader(std::move(languageModLoader));
    }

    void PalMainLoader::SetupAutoReload()
    {
        auto config = PS::PSConfig::Get();
        if (!config->IsAutoReloadEnabled()) return;

        PS::Log<LogLevel::Normal>(STR("Auto-reload is enabled.\n"));

        auto modsPath = GetModsPath();

        m_fileWatcher = std::make_unique<PS::FileWatchWrapper>(modsPath, [this](efsw::WatchID watchId, const std::string& dir,
            const std::string& filename, efsw::Action action,
            std::string oldFilename) {
                if (action == efsw::Actions::Add || action == efsw::Actions::Modified)
                {
                    auto path = fs::path(dir) / filename;
                    AutoReload(path);
                }
            }
        );
        m_fileWatcher->Watch();
    }

    void PalMainLoader::SetupAlternativePakPathReader()
    {
        auto GetPakFolders_Address = Palworld::SignatureManager::GetSignature("FPakPlatformFile::GetPakFolders");
        if (GetPakFolders_Address)
        {
            GetPakFolders_Hook = safetyhook::create_inline(reinterpret_cast<void*>(GetPakFolders_Address),
                GetPakFolders);
        }
        else
        {
            PS::Log<LogLevel::Error>(STR("Unable to setup additional .pak read directory, signature for FPakPlatformFile::GetPakFolders is outdated.\n"));
        }
    }

    void PalMainLoader::InitCore()
    {
        if (m_hasInit) return;
        m_hasInit = true;

        PS::Log<LogLevel::Verbose>(STR("Initializing Static Class Storage...\n"));
        Palworld::StaticClassStorage::Initialize();

        SetupPostEngineInitLoaders();

        HookGameInstanceInit();

        HookInitGameState();

        PS::Log<LogLevel::Verbose>(STR("Initialized Core\n"));
    }

    void PalMainLoader::RegisterLoader(std::unique_ptr<PalModLoaderBase> newLoader)
    {
        newLoader->AssignDatatableRegistry(m_datatableRegistry);
        newLoader->Setup();

        m_loaders.push_back(std::move(newLoader));
    }

    void PalMainLoader::InitializeMods(EEngineLifecyclePhase engineLifecyclePhase)
    {
        for (auto& loader : m_loaders)
        {
#ifdef __linux__
            // Linux-Portierung: Fortschritt VOR dem Aufruf protokollieren. Faellt ein
            // Loader in einen Segfault, steht im Log, welcher es war -- UE4SS' Handler
            // bricht die Init sonst kommentarlos ab.
            PS::Log<LogLevel::Normal>(STR("Initializing loader '{}' ..."), RC::to_generic_string(loader->GetModFolderType()));
#endif
            loader->Initialize(engineLifecyclePhase);
        }
    }

    void PalMainLoader::LoadMods(EEngineLifecyclePhase engineLifecyclePhase)
    {
        IterateModsFolder([&](const fs::path& modPath, const RC::StringType& modName)
        {
            try
            {
                PS::Log<RC::LogLevel::Normal>(STR("Loading mod: {}\n"), modName);

                for (auto& loader : m_loaders)
                {
                    loader->Load(modPath, modName, engineLifecyclePhase);
                }
            }
            catch (const std::exception& e)
            {
                PS::Log<LogLevel::Error>(STR("Failed to load mod {} - {}\n"), modName, RC::to_generic_string(e.what()));
            }
        });
    }

    std::filesystem::path PalMainLoader::GetModsPath()
    {
        static auto modsPath = fs::path(UE4SSProgram::get_program().get_working_directory()) / "Mods" / "PalSchema" / "mods";
        return modsPath;
    }

    // This entire function block will get called twice, it's fine.
    void PalMainLoader::GetPakFolders(const TCHAR* CmdLine, TArray<FString>* OutPakFolders)
    {
        PS::Log<LogLevel::Verbose>(STR("Calling original FPakPlatformFile::GetPakFolders...\n"));
        GetPakFolders_Hook.call(CmdLine, OutPakFolders);

        try
        {
            // Calling this here, because we want GMalloc to be available ASAP inside this hook so we can make our changes to the TArray.
            // Once UE4SS starts running things on Game Thread, this could be moved to PreInitialize.
            // Just for clarity, there is a check inside InitializeGMalloc to prevent it from running twice since GetPakFolders runs twice.
            UnrealOffsets::InitializeGMalloc();
        }
        catch (const std::exception& e)
        {
            PS::Log<LogLevel::Error>(STR("Failed to initialize GMalloc early: {}\n"), RC::to_generic_string(e.what()));
            PS::Log<LogLevel::Error>(STR("PalSchema won't be able to load paks from the PalSchema/mods folder.\n"));
            return;
        }
        
        PS::Log<LogLevel::Verbose>(STR("Preparing to add extra .pak read directory...\n"));
        auto ModsFolderPath = GetModsPath();
        auto AbsolutePath = ModsFolderPath.native();
        auto AbsolutePathWithSuffix = PS::Format("{}/", RC::to_generic_string(AbsolutePath));

        PS::Log<LogLevel::Verbose>(STR("Setting extra .pak read directory to {}\n"), AbsolutePathWithSuffix);

        // If GMalloc isn't properly initialized, accessing the TArray will crash.
        OutPakFolders->Add(FString(AbsolutePathWithSuffix.c_str()));

        PS::Log<LogLevel::Verbose>(STR("Added extra .pak read directory at {}\n"), AbsolutePathWithSuffix);
    }

    void PalMainLoader::OnDataTableSerialized(RC::Unreal::UDataTable* This, RC::Unreal::FArchive* Archive)
    {
        DatatableSerialize_Hook.call(This, Archive);

        for (auto& Callback : DatatableSerializeCallbacks)
        {
            Callback(This);
        }
    }

    void PalMainLoader::OnInitGameState(RC::Unreal::UObject* This)
    {
        // Signatur von AGameModeBase::InitGameState ist "void InitGameState()" --
        // nur 'this'. Rueckgabetyp void passt damit zur Zielfunktion.
        InitGameState_Hook.call(This);

        for (auto& Callback : InitGameStateCallbacks)
        {
            Callback(This);
        }
    }

    void PalMainLoader::OnGameInstanceInit(RC::Unreal::UObject* This)
    {
        GameInstanceInit_Hook.call(This);

        for (auto& Callback : GameInstanceInitCallbacks)
        {
            Callback(This);
        }
    }
}
