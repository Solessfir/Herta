#include "Herta/Localization/Localization.h"

#include "Herta/Assets/VirtualFileSystem.h"
#include "Herta/Core/Json.h"

#include <simdjson.h>

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <unordered_map>
#include <utility>

namespace Herta
{
namespace
{
constexpr std::string_view CatalogFormat = "HertaTextCatalog";
constexpr std::uint64_t CatalogVersion = 1;
constexpr std::size_t MaximumCatalogBytes = std::size_t{16} * 1024 * 1024;

[[nodiscard]] std::unexpected<FLocalizationError> Failed(std::string Message)
{
	return std::unexpected(FLocalizationError{std::move(Message)});
}

[[nodiscard]] std::string FullKey(const FTextCatalog& Catalog, const std::string_view Key)
{
	return Catalog.Namespace + "." + std::string(Key);
}

[[nodiscard]] bool IsValidName(const std::string_view Name)
{
	return !Name.empty() && std::ranges::all_of(Name, [](const char Character)
	{
		return (Character >= 'a' && Character <= 'z') || (Character >= 'A' && Character <= 'Z') || (Character >= '0' && Character <= '9') || Character == '_' || Character == '.' || Character == '-';
	});
}

struct FCompiledCulture
{
	std::string Name{};
	std::unordered_map<std::uint64_t, FMessage> Messages{};
};
}

struct FLocalization::FState
{
	// The source culture first; the pseudo culture is derived from it and never loaded.
	std::vector<FCompiledCulture> Cultures{};
	FCompiledCulture Pseudo;
	std::string CultureName = std::string(SourceCulture);
	const FCulture* Culture = FCulture::Find(SourceCulture);
	std::uint64_t Generation = 1;
};

std::expected<FTextCatalog, FLocalizationError> ParseTextCatalog(const std::string_view Json)
{
	if (Json.empty() || Json.size() > MaximumCatalogBytes)
	{
		return Failed("A text catalog is empty or exceeds 16 MiB");
	}

	simdjson::dom::parser Parser;
	simdjson::dom::element Root;
	simdjson::dom::object Object;
	std::string_view Format, Culture, Namespace;
	std::uint64_t Version = 0;
	simdjson::dom::array Messages;
	if (Parser.parse(Json.data(), Json.size()).get(Root) || Root.get_object().get(Object) || Object["format"].get_string().get(Format) || Format != CatalogFormat
	    || Object["version"].get_uint64().get(Version) || Version != CatalogVersion || Object["culture"].get_string().get(Culture) || Object["namespace"].get_string().get(Namespace)
	    || Object["messages"].get_array().get(Messages) || Object.size() != 5)
	{
		return Failed("A text catalog has an invalid header or an unsupported version");
	}

	if (!IsValidName(Culture) || !IsValidName(Namespace))
	{
		return Failed(std::format("A text catalog names an invalid culture '{}' or namespace '{}'", Culture, Namespace));
	}

	FTextCatalog Catalog{.Culture = std::string(Culture), .Namespace = std::string(Namespace)};
	Catalog.Messages.reserve(Messages.size());
	for (const simdjson::dom::element Element : Messages)
	{
		simdjson::dom::object Message;
		std::string_view Key, Text, Comment;
		if (Element.get_object().get(Message) || Message["key"].get_string().get(Key) || Message["text"].get_string().get(Text))
		{
			return Failed(std::format("Catalog {}/{} has a message without a key and text", Culture, Namespace));
		}

		const bool bHasComment = !Message["comment"].get_string().get(Comment);
		if (Message.size() != (bHasComment ? 3u : 2u) || !IsValidName(Key))
		{
			return Failed(std::format("Catalog {}/{} has an invalid message '{}'", Culture, Namespace, Key));
		}

		Catalog.Messages.push_back({.Key = std::string(Key), .Text = std::string(Text), .Comment = std::string(Comment)});
	}

	return Catalog;
}

std::expected<std::vector<FTextCatalog>, FLocalizationError> LoadTextCatalogs(const FVirtualFileSystem& Files, const std::filesystem::path& Directory)
{
	std::vector<FTextCatalog> Catalogs;
	const std::filesystem::path Base = (Files.GetRoot() / Directory).lexically_normal();
	for (const std::filesystem::path& Path : Files.FindFiles(".hloc", 5))
	{
		const std::filesystem::path Relative = Path.lexically_normal().lexically_relative(Base);
		if (Relative.empty() || *Relative.begin() == ".." || std::distance(Relative.begin(), Relative.end()) != 2)
		{
			continue;
		}

		const auto Bytes = Files.Read(Path);
		if (!Bytes)
		{
			return Failed(Bytes.error().Message);
		}

		auto Catalog = ParseTextCatalog({reinterpret_cast<const char*>(Bytes->data()), Bytes->size()});
		if (!Catalog)
		{
			return Failed(std::format("{}: {}", Path.generic_string(), Catalog.error().Message));
		}

		if (Catalog->Culture != Relative.begin()->generic_string() || Catalog->Namespace != Relative.stem().generic_string())
		{
			return Failed(std::format("{} names culture {} and namespace {}, unlike its path", Path.generic_string(), Catalog->Culture, Catalog->Namespace));
		}

		Catalogs.push_back(std::move(*Catalog));
	}

	return Catalogs;
}

std::string SerializeTextCatalog(const FTextCatalog& Catalog)
{
	std::vector<const FCatalogMessage*> Messages;
	Messages.reserve(Catalog.Messages.size());
	for (const FCatalogMessage& Message : Catalog.Messages)
	{
		Messages.push_back(&Message);
	}

	std::ranges::sort(Messages, {}, &FCatalogMessage::Key);
	std::string Json = std::format("{{\n  \"format\": \"{}\",\n  \"version\": {},\n  \"culture\": ", CatalogFormat, CatalogVersion);
	AppendJsonString(Json, Catalog.Culture);
	Json += ",\n  \"namespace\": ";
	AppendJsonString(Json, Catalog.Namespace);
	Json += ",\n  \"messages\": [";
	for (std::size_t Index = 0; Index < Messages.size(); ++Index)
	{
		Json += Index == 0 ? "\n    {\"key\": " : ",\n    {\"key\": ";
		AppendJsonString(Json, Messages[Index]->Key);
		Json += ", \"text\": ";
		AppendJsonString(Json, Messages[Index]->Text);
		if (!Messages[Index]->Comment.empty())
		{
			Json += ", \"comment\": ";
			AppendJsonString(Json, Messages[Index]->Comment);
		}

		Json += "}";
	}

	Json += Messages.empty() ? "]\n}\n" : "\n  ]\n}\n";
	return Json;
}

std::vector<std::string> ValidateTranslation(const FTextCatalog& Source, const FTextCatalog& Translation)
{
	std::vector<std::string> Problems;
	if (Source.Namespace != Translation.Namespace)
	{
		Problems.push_back(std::format("Catalog {}/{} does not translate namespace {}", Translation.Culture, Translation.Namespace, Source.Namespace));
		return Problems;
	}

	std::unordered_map<std::string_view, const FCatalogMessage*> SourceMessages;
	for (const FCatalogMessage& Message : Source.Messages)
	{
		SourceMessages.emplace(Message.Key, &Message);
	}

	std::unordered_map<std::string_view, int> Seen;
	for (const FCatalogMessage& Message : Translation.Messages)
	{
		const std::string Where = std::format("{}/{}.{}", Translation.Culture, Translation.Namespace, Message.Key);
		if (++Seen[Message.Key] == 2)
		{
			Problems.push_back(Where + " appears more than once");
		}

		const auto Compiled = FMessage::Compile(Message.Text);
		if (!Compiled)
		{
			Problems.push_back(std::format("{}: {}", Where, Compiled.error().Message));
			continue;
		}

		if (&Translation == &Source)
		{
			continue;
		}

		const auto Found = SourceMessages.find(Message.Key);
		if (Found == SourceMessages.end())
		{
			Problems.push_back(Where + " is not in the source catalog");
			continue;
		}

		const auto SourceCompiled = FMessage::Compile(Found->second->Text);
		if (SourceCompiled && SourceCompiled->GetArgumentNames() != Compiled->GetArgumentNames())
		{
			Problems.push_back(Where + " uses different arguments than its source");
		}
	}

	return Problems;
}

std::string Pseudolocalize(const std::string_view Text)
{
	constexpr std::string_view Plain = "aeiouAEIOUcnszyCNSZY";
	constexpr std::array<std::string_view, 20> Accented{
	    "á", "é", "í", "ó", "ú", "Á", "É", "Í", "Ó", "Ú",
	    "ç", "ñ", "š", "ž", "ý", "Ç", "Ñ", "Š", "Ž", "Ý",
	};

	std::string Output;
	Output.reserve(Text.size() * 2);
	for (const char Character : Text)
	{
		const std::size_t Index = Plain.find(Character);
		if (Index == std::string_view::npos)
		{
			Output.push_back(Character);
			continue;
		}

		Output += Accented[Index];
		// Doubling vowels grows text by about a third, like longer translations do.
		if (Index < 10)
		{
			Output += Accented[Index];
		}
	}

	return Output;
}

FLocalization::FLocalization()
    : State(std::make_unique<FState>())
{
}

FLocalization::~FLocalization() = default;
FLocalization::FLocalization(FLocalization&&) noexcept = default;
FLocalization& FLocalization::operator=(FLocalization&&) noexcept = default;

std::expected<void, FLocalizationError> FLocalization::SetCatalogs(const std::span<const FTextCatalog> Catalogs)
{
	std::unordered_map<std::string, const FTextCatalog*> Sources;
	for (const FTextCatalog& Catalog : Catalogs)
	{
		if (Catalog.Culture == SourceCulture && !Sources.emplace(Catalog.Namespace, &Catalog).second)
		{
			return Failed(std::format("Namespace {} has two source catalogs", Catalog.Namespace));
		}

		if (Catalog.Culture != SourceCulture && !FCulture::Find(Catalog.Culture))
		{
			return Failed(std::format("Catalog {}/{} is for a culture Herta has no data for", Catalog.Culture, Catalog.Namespace));
		}
	}

	std::vector<FCompiledCulture> Cultures(1);
	Cultures.front().Name = std::string(SourceCulture);
	FCompiledCulture Pseudo{.Name = std::string(PseudoCulture)};
	std::unordered_map<std::uint64_t, std::string> Keys;
	for (const FTextCatalog& Catalog : Catalogs)
	{
		const auto Source = Sources.find(Catalog.Namespace);
		if (Source == Sources.end())
		{
			return Failed(std::format("Catalog {}/{} has no source catalog", Catalog.Culture, Catalog.Namespace));
		}

		if (const auto Problems = ValidateTranslation(*Source->second, Catalog); !Problems.empty())
		{
			return Failed(Problems.front());
		}

		auto Compiled = std::ranges::find(Cultures, Catalog.Culture, &FCompiledCulture::Name);
		if (Compiled == Cultures.end())
		{
			Compiled = Cultures.insert(Cultures.end(), FCompiledCulture{.Name = Catalog.Culture});
		}

		for (const FCatalogMessage& Message : Catalog.Messages)
		{
			std::string Key = FullKey(Catalog, Message.Key);
			const std::uint64_t Hash = HashTextKey(Key);
			if (const auto [Existing, bInserted] = Keys.emplace(Hash, Key); !bInserted && Existing->second != Key)
			{
				return Failed(std::format("Text keys {} and {} have the same hash", Existing->second, Key));
			}

			FMessage Text = *FMessage::Compile(Message.Text);
			if (Catalog.Culture == SourceCulture)
			{
				Pseudo.Messages.insert_or_assign(Hash, Text.TransformText(&Pseudolocalize));
			}

			Compiled->Messages.insert_or_assign(Hash, std::move(Text));
		}
	}

	State->Cultures = std::move(Cultures);
	State->Pseudo = std::move(Pseudo);
	++State->Generation;
	return {};
}

bool FLocalization::SetCulture(const std::string_view Tag)
{
	const bool bPseudo = Tag == PseudoCulture;
	const FCulture* const Culture = FCulture::Find(bPseudo ? SourceCulture : Tag);
	if (!Culture)
	{
		return false;
	}

	State->CultureName = bPseudo ? std::string(PseudoCulture) : std::string(Culture->GetName());
	State->Culture = Culture;
	++State->Generation;
	return true;
}

std::string_view FLocalization::GetCultureName() const noexcept
{
	return State->CultureName;
}

const FCulture& FLocalization::GetCulture() const noexcept
{
	return *State->Culture;
}

std::vector<std::string> FLocalization::GetAvailableCultures() const
{
	std::vector<std::string> Names;
	for (const FCompiledCulture& Culture : State->Cultures)
	{
		Names.push_back(Culture.Name);
	}

	return Names;
}

std::uint64_t FLocalization::GetGeneration() const noexcept
{
	return State->Generation;
}

std::string FLocalization::Format(const FTextId Id, const std::span<const FMessageArgument> Arguments) const
{
	return FormatIn(State->CultureName, Id, Arguments);
}

std::string FLocalization::FormatIn(const std::string_view CultureTag, const FTextId Id, const std::span<const FMessageArgument> Arguments) const
{
	const auto FindIn = [&Id](const FCompiledCulture& Culture) -> const FMessage*
	{
		const auto Found = Culture.Messages.find(Id.Hash);
		return Found != Culture.Messages.end() ? &Found->second : nullptr;
	};

	const bool bPseudo = CultureTag == PseudoCulture;
	const FCulture* const Culture = FCulture::Find(bPseudo ? SourceCulture : CultureTag);
	const FMessage* Message = bPseudo ? FindIn(State->Pseudo) : nullptr;
	for (const FCulture* Parent = Culture; !Message && Parent; Parent = Parent->GetParent())
	{
		const auto Compiled = std::ranges::find(State->Cultures, Parent->GetName(), &FCompiledCulture::Name);
		Message = Compiled != State->Cultures.end() ? FindIn(*Compiled) : nullptr;
	}

	if (!Message && !State->Cultures.empty())
	{
		Message = FindIn(State->Cultures.front());
	}

	if (!Message)
	{
		return std::string(Id.Key);
	}

	std::string Text = Message->Format(Culture ? *Culture : *State->Culture, Arguments);
	return bPseudo ? "[" + Text + "]" : Text;
}
}
