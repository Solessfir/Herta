#pragma once

#include "Herta/Localization/Localization.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
struct FLocalizationReport
{
	// Anything that keeps the catalogs from loading: invalid messages, translations of keys the source lacks, or differing arguments.
	std::vector<std::string> Errors{};
	// Source messages a culture has no translation for; they show in the source language.
	std::vector<std::string> Missing{};
	// Catalogs whose files are not in canonical form, so saving them would change their bytes.
	std::vector<std::string> NonCanonical{};
};

// Checks a whole set of catalogs as FLocalization::SetCatalogs would load it, and reports what translators still have to do.
[[nodiscard]] FLocalizationReport ValidateCatalogs(std::span<const FTextCatalog> Catalogs, std::span<const std::string> FileTexts = {});
// A C++ header of constexpr FTextId constants, one per source message, in namespace Herta::Text::<Namespace>. Dots in keys become
// underscores: Menu.Resume is Herta::Text::Game::Menu_Resume.
[[nodiscard]] std::string GenerateTextIdHeader(const FTextCatalog& Source, std::string_view CatalogPath);
}
