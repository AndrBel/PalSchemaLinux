cmake_minimum_required(VERSION 3.22)

set(CMAKE_CXX_STANDARD 23)

add_subdirectory(deps)

set(TARGET PalSchema)
project(${TARGET})

file(GLOB SRC_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dllmain.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Generator/JsonSchema/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Loader/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Loader/Blueprint/PalBlueprintMod.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Loader/Spawner/PalListInfo.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Loader/Spawner/PalSpawnGroupListInfo.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Loader/Spawner/SpawnerInfo.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Loader/WildcardFilter/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/SDK/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/SDK/Classes/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/SDK/Classes/Custom/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/SDK/Classes/Custom/DataTable/TableSerializer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/SDK/Classes/Property/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/SDK/Helper/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/SDK/Structs/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/SDK/Structs/Reflected/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/SDK/Structs/Custom/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/SDK/Structs/Custom/DataTable/FTableSerializerRow.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Tools/EnumSchemaDefinitionGenerator.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Utility/Config.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Utility/JsonHelpers.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Utility/EnumHelpers.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Misc/FileWatchWrapper.cpp"
)

add_library(${TARGET} SHARED ${SRC_FILES})
# Linux/GCC-14: UE4SS-Header loesen -Wchanges-meaning aus (kein echter Fehler)
target_compile_options(${TARGET} PRIVATE -Wno-changes-meaning -Wno-error)
# Serverbuild ohne ImGui (UE4SS mit UE4SS_GUI_ENABLED=OFF)
# PS_DATATABLE_SERIALIZE_SLOT ist ein ROH-Index in das _ZTV-Symbol,
# also inklusive der zwei Itanium-Kopfeintraege (offset-to-top, typeinfo):
#   Rohindex = Funktionszeiger-Index + 2.
# Verifiziert an PalServer-Linux-Shipping:
#   fnptr 27 (roh 29) = 0xa302f30 = UDataTable::Serialize(FArchive&)
#     -- nimmt (this, FArchive*), ruft UObject::Serialize (0x7af57c0)
#     und wird von der FStructuredArchive-Ueberladung (fnptr 26) mit
#     GetUnderlyingArchive() aufgerufen.
#   fnptr 34 (roh 36) = 0x4d07f50 = UDataTable::NeedsLoadForClient()
#     -- prueft bStripFromClientBuilds (Offset 0x80) und liefert bool.
# Der bisherige Wert 36 legte den void-Detour OnDataTableSerialized auf
# diese bool-Funktion: Rueckgabewert in AL war danach Muell und der
# FArchive*-Parameter Registermuell.
target_compile_definitions(${TARGET} PRIVATE PS_HEADLESS_NO_GUI PS_DATATABLE_SERIALIZE_SLOT=29)
target_include_directories(${TARGET} PRIVATE 
    include/
)
target_link_libraries(${TARGET} PRIVATE Zydis Zycore safetyhook nlohmann_json::nlohmann_json glaze::glaze UE4SS efsw)

if (UNIX)
    # Linux-Portierung: im Serverprozess liegen DREI C++-Laufzeiten uebereinander.
    #   * PalServer-Linux-Shipping ist statisch gegen LLVM libc++/libc++abi gebaut
    #     (798 _ZNSt3__1*-Symbole) und exportiert __cxa_throw,
    #     __cxa_allocate_exception, __cxa_begin_catch, _ZTISt13runtime_error.
    #   * libsteam_api.so (DT_NEEDED des Spiels) exportiert zusaetzlich
    #     __gxx_personality_v0 und die __cxxabiv1-typeinfos aus einer statisch
    #     gelinkten libstdc++.
    #   * PalSchema selbst wird mit GCC/libstdc++ uebersetzt.
    # Die globale Symbolreihenfolge (Executable vor allem anderen) laesst ein
    # 'throw' aus PalSchema in libc++abis __cxa_throw laufen (exception class
    # "CLNGC++"), waehrend die Landing Pads __gxx_personality_v0 (GNU, aus
    # libsteam_api) benutzen. Die Exception gilt dort als __foreign_exception und
    # der Prozess stirbt mit SIGSEGV -- nachgewiesen beim ersten echten throw
    # (GetDatatableByName("DT_PalMonsterParameter")), rsi zeigte auf
    # _ZTIN10__cxxabiv119__foreign_exceptionE in libsteam_api.so.
    # LD_PRELOAD der System-libstdc++ genuegt NICHT, weil das Executable in der
    # Suchreihenfolge vor jedem Preload steht.
    #
    # Getestet und VERWORFEN (jeweils sofortiger Absturz beim Mod-Start):
    # haben mehr kaputt gemacht als geholfen und sind deshalb zurueckgenommen:
    #   -static-libstdc++ -static-libgcc -Wl,-Bsymbolic
    #       -> bindet auch die von libUnreal.so/libUE4SS.so GETEILTEN Globals
    #          (Container::UnrealObjectVC, Offset-Tabellen) an leere Kopien in
    #          main.so. Sofortiger SIGSEGV beim Mod-Start (fault=0xb0, rdi=0xa8),
    #          "Mod 'PalSchema' crashed during startup".
    #   -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL
    #       -> versteckt Symbole ALLER statischen Archive (auch Zydis/Zycore, die
    #          libsafetyhook.so ebenfalls benutzt). Ergebnis: "free(): invalid
    #          pointer" (SIGABRT) beim Mod-Start.
    #   -static-libstdc++ -static-libgcc -Wl,--exclude-libs,libstdc++.a:libgcc.a:libgcc_eh.a
    #       -> ebenfalls SIGABRT beim Mod-Start. PalSchema und libUE4SS.so/libUnreal.so
    #          reichen staendig std::string/std::function ueber die Modulgrenze; eine
    #          zweite, private libstdc++ in main.so ist damit nicht vertraeglich.
    # Konsequenz: KEINE Linker-Gegenmassnahme aktiv. Der Fehler bleibt latent,
    # solange PalSchema kein 'throw' ausfuehrt -- die GameInstanceInit-Loader
    # verzichten deshalb bewusst auf Exceptions (siehe PalModLoaderBase).
endif()
