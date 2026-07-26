#include "Herta/Platform/Platform.h"

namespace Herta
{
std::string_view GetPlatformName(const EPlatform Platform) noexcept
{
	switch (Platform)
	{
		case EPlatform::Windows:
			return "Windows";
		case EPlatform::Linux:
			return "Linux";
	}

	return "Unknown";
}
}
