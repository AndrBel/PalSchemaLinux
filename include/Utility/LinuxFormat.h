#pragma once
// Linux-Portierung: std::format unterstuetzt nur char und wchar_t, aber UE4SS
// verwendet unter Linux CharType = char16_t (FORCE_U16 in StringType.hpp).
// Loesung: Argumente nach UTF-8 verengen, dort formatieren, Ergebnis zurueck
// nach StringType wandeln. RC::to_wstring() liefert unter Linux u16string.
#include <format>
#include <string>
#include <tuple>
#include <type_traits>
#include <String/StringType.hpp>
#include <Helpers/String.hpp>

namespace PS {
    template <typename T>
    auto NarrowArg(T&& value)
    {
        using D = std::decay_t<T>;
        if constexpr (std::is_same_v<D, RC::StringType> || std::is_same_v<D, RC::StringViewType> ||
                      std::is_same_v<D, const RC::CharType*> || std::is_same_v<D, RC::CharType*>)
        {
            return RC::to_string(RC::StringType(value));
        }
        else if constexpr (std::is_same_v<D, std::wstring> || std::is_same_v<D, std::wstring_view>)
        {
            // Unter Linux ist wchar_t 32 Bit und nicht CharType — erst nach
            // StringType (char16_t) wandeln, dann nach UTF-8 verengen.
            return RC::to_string(RC::to_wstring(std::wstring(value)));
        }
        else
        {
            return std::forward<T>(value);
        }
    }

    template <typename... Args>
    auto Format(std::string_view fmt, Args&&... args) -> RC::StringType
    {
        auto narrowed = std::make_tuple(NarrowArg(std::forward<Args>(args))...);
        auto narrow_result = std::apply(
            [&](auto const&... unpacked) {
                return std::vformat(fmt, std::make_format_args(unpacked...));
            },
            narrowed);
        return RC::to_wstring(narrow_result);
    }
}
