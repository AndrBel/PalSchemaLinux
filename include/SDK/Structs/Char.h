// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Utility/LinuxCompat.h"
#include <ctype.h>
#include <wctype.h>
#include <type_traits>
#include "Unreal/Core/Traits/IntType.hpp"

namespace UECustom {
	/*-----------------------------------------------------------------------------
	Character type functions.
-----------------------------------------------------------------------------*/

	template <typename CharType, const unsigned int Size>
	struct TCharBase
	{
		static constexpr CharType LineFeed = (CharType)0xa;
		static constexpr CharType VerticalTab = (CharType)0xb;
		static constexpr CharType FormFeed = (CharType)0xc;
		static constexpr CharType CarriageReturn = (CharType)0xd;
		static constexpr CharType NextLine = (CharType)0x85;
		static constexpr CharType LineSeparator = (CharType)0x2028;
		static constexpr CharType ParagraphSeparator = (CharType)0x2029;
	};

	template <typename CharType>
	struct TCharBase<CharType, 1>
	{
		static constexpr CharType LineFeed = (CharType)0xa;
		static constexpr CharType VerticalTab = (CharType)0xb;
		static constexpr CharType FormFeed = (CharType)0xc;
		static constexpr CharType CarriageReturn = (CharType)0xd;
	};

	/**
	 * TChar
	 * Set of utility functions operating on a single character. The functions
	 * are specialized for ANSICHAR and TCHAR character types. You can use the
	 * typedefs FChar and FCharAnsi for convenience.
	 */
	template <typename CharType>
	struct TChar : TCharBase<CharType, sizeof(CharType)>
	{
		static constexpr FORCEINLINE RC::Unreal::uint32 ToUnsigned(CharType Char)
		{
			return (typename RC::Unreal::TUnsignedIntType<sizeof(CharType)>::Type)Char;
		}
	};
}