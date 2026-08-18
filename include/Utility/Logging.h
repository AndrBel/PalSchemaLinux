#pragma once

#include <HAL/Platform.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include "Utility/Config.h"

namespace PS {
    // Linux-Portierung: std::format unterstuetzt kein char16_t, und UE4SS nutzt
    // unter Linux CharType = char16_t (FORCE_U16 in StringType.hpp).
    // std::format wurde hier ohnehin nur benutzt, um ein Praefix voranzustellen —
    // die eigentliche Formatierung der Argumente macht Output::send. Also reicht
    // schlichte Verkettung, und die funktioniert auf beiden Plattformen.
    template <RC::Unreal::int32 optional_arg, typename... FmtArgs>
    auto Log(RC::File::StringViewType content, FmtArgs... fmt_args) -> void
    {
        if (optional_arg == RC::LogLevel::Error)
        {
            auto formatted_log = RC::StringType(STR("[PalSchema] [error] ")) + RC::StringType(content);
            RC::Output::send<optional_arg>(formatted_log, fmt_args...);
        }
        else if (optional_arg == RC::LogLevel::Warning)
        {
            auto formatted_log = RC::StringType(STR("[PalSchema] [warning] ")) + RC::StringType(content);
            RC::Output::send<optional_arg>(formatted_log, fmt_args...);
        }
        else if (optional_arg == RC::LogLevel::Verbose)
        {
            auto config = PS::PSConfig::Get();
            if (!config->IsDebugLoggingEnabled()) return;

            auto formatted_log = RC::StringType(STR("[PalSchema] [debug] ")) + RC::StringType(content);
            RC::Output::send<optional_arg>(formatted_log, fmt_args...);
        }
        else
        {
            auto formatted_log = RC::StringType(STR("[PalSchema] ")) + RC::StringType(content);
            RC::Output::send<optional_arg>(formatted_log, fmt_args...);
        }
    }
}
