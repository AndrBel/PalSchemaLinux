#include "SDK/Classes/KismetInternationalizationLibrary.h"
#include "SDK/Classes/Custom/UObjectGlobals.h"
#include "Unreal/UFunction.hpp"

using namespace RC;
using namespace RC::Unreal;

namespace Palworld {
	FString UKismetInternationalizationLibrary::GetCurrentLanguage()
	{
		static auto Function = UECustom::UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, TEXT("/Script/Engine.KismetInternationalizationLibrary:GetCurrentLanguage"));

        if (!Function)
        {
            return FString{};
        }

		// Linux-Portierung: Der von ProcessEvent geschriebene FString gehoert dem
		// SPIEL-Heap. Laesst man ihn hier regulaer zerstoeren, laeuft das ueber
		// UE4SS' FMemory::Free, das unter Linux schlicht ::free() aufruft -- auf
		// einen Zeiger aus einem fremden Allokator. Ergebnis war ein
		// "munmap_chunk(): invalid pointer" (SIGABRT) im 'translations'-Loader,
		// dem letzten Loader der GameInstanceInit-Phase.
		// Deshalb: Rohspeicher als Parameterblock benutzen, den Text
		// herauskopieren und den Engine-FString bewusst NICHT destruieren. Die
		// Allokation wird einmalig pro Prozess geleakt (wenige Bytes).
		alignas(FString) unsigned char ParamsStorage[sizeof(FString)]{};

		GetDefaultObj()->ProcessEvent(Function, static_cast<void*>(ParamsStorage));

		auto* Returned = reinterpret_cast<FString*>(ParamsStorage);
		const TCHAR* Chars = Returned->GetCharArray().GetData();
		if (!Chars)
		{
			return FString{};
		}

		return FString{ Chars };
	}

	UKismetInternationalizationLibrary* UKismetInternationalizationLibrary::GetDefaultObj()
	{
		static auto Self = UECustom::UObjectGlobals::StaticFindObject<UKismetInternationalizationLibrary*>(nullptr, nullptr, TEXT("/Script/Engine.Default__KismetInternationalizationLibrary"));
		return Self;
	}
}