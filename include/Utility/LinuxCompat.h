#pragma once
// Linux-Portierung: Unreal definiert TCHAR nur in Windows/WindowsPlatform.hpp.
// Unter Linux nutzt UE4SS CharType (char16_t) als Zeichentyp.
#ifdef __linux__
#include "Utility/LinuxCompat.h"
#include <String/StringType.hpp>
using TCHAR = RC::CharType;
#endif
