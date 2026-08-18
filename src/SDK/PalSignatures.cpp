#include "SDK/PalSignatures.h"
#ifdef __linux__
#include "Utility/LinuxVTable.h"
#endif
#include "Signatures.hpp"
#include "SigScanner/SinglePassSigScanner.hpp"
#include "Utility/Logging.h"
#include "Helpers/String.hpp"
#include "ASMHelper/ASMHelper.hpp"

using namespace RC;
using namespace RC::Unreal;

namespace Palworld {
    void SignatureManager::Initialize()
    {
        std::vector<SignatureContainer> SigContainerBox;
        SinglePassScanner::SignatureContainerMap SigContainerMap;

#ifdef __linux__
        // Zuerst alles aufloesen, was ueber ein exportiertes _ZTV-Symbol exakt
        // bestimmbar ist. Diese Namen werden danach vom AOB-Scan ausgenommen --
        // sonst meldet der Scan sie faelschlich als "outdated".
        for (auto& [ClassAndName, VTableEntry] : LinuxVTableSignatures)
        {
            void* FunctionPointer = PS::LinuxVTable::GetVirtualFunction(VTableEntry.first.c_str(), VTableEntry.second);
            if (!FunctionPointer)
            {
                PS::Log<LogLevel::Error>(STR("Unable to resolve {} via vtable symbol {}."),
                    RC::to_generic_string(ClassAndName), RC::to_generic_string(VTableEntry.first));
                continue;
            }

            SignatureMap.emplace(ClassAndName, FunctionPointer);
            PS::Log<LogLevel::Normal>(STR("Found {}: {} (vtable slot {})."),
                RC::to_generic_string(ClassAndName), FunctionPointer, VTableEntry.second - 2);
        }
#endif

        for (auto& [ClassAndName, Signature] : Signatures)
        {
            if (SignatureMap.find(ClassAndName) != SignatureMap.end()) continue;

            SignatureContainer SigContainer = [=]() -> SignatureContainer {
                return {
                    {{Signature}},
                    [=](SignatureContainer& self) {
                        void* FunctionPointer = static_cast<void*>(self.get_match_address());

                        SignatureMap.emplace(ClassAndName, FunctionPointer);
                        PS::Log<LogLevel::Normal>(STR("Found {}: {}\n"), RC::to_generic_string(ClassAndName), FunctionPointer);

                        self.get_did_succeed() = true;

                        return true;
                    },
                    [=](const SignatureContainer& self) {
                        if (!self.get_did_succeed())
                        {
                            PS::Log<RC::LogLevel::Error>(STR("Failed to find signature for {}.\n"), RC::to_generic_string(ClassAndName));
                        }
                    }
                };
            }();
            SigContainerBox.emplace_back(SigContainer);
        }

        for (auto& [ClassAndName, Signature] : SignaturesCallResolve)
        {
            if (SignatureMap.find(ClassAndName) != SignatureMap.end()) continue;

            SignatureContainer SigContainer = [=]() -> SignatureContainer {
                return {
                    {{Signature}},
                    [=](SignatureContainer& self) {
                        void* FunctionPointer = static_cast<void*>(self.get_match_address());
                        void* FinalAddress = ASM::resolve_call(FunctionPointer);

                        SignatureMap.emplace(ClassAndName, FinalAddress);
                        PS::Log<LogLevel::Normal>(STR("Found {}: {}\n"), RC::to_generic_string(ClassAndName), FinalAddress);

                        self.get_did_succeed() = true;

                        return true;
                    },
                    [=](const SignatureContainer& self) {
                        if (!self.get_did_succeed())
                        {
                            PS::Log<RC::LogLevel::Error>(STR("Failed to find signature for {}.\n"), RC::to_generic_string(ClassAndName));
                        }
                    }
                };
            }();
            SigContainerBox.emplace_back(SigContainer);
        }

        SigContainerMap.emplace(ScanTarget::MainExe, SigContainerBox);
        SinglePassScanner::start_scan(SigContainerMap);
    }

    void* SignatureManager::GetSignature(const std::string& ClassAndFunction)
    {
        auto It = SignatureMap.find(ClassAndFunction);
        if (It != SignatureMap.end())
        {
            return It->second;
        }

        return nullptr;
    }
}