#include "Herta/AssetPipeline/GameStaging.h"
#include "Herta/AssetPipeline/ModStaging.h"
#include "Herta/Assets/ContentPackage.h"
#include "Herta/Assets/VirtualFileSystem.h"
#include "Herta/PackageBuilder/PackageBuilder.h"
#include "Herta/Platform/Platform.h"
#include "Herta/Platform/Process.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Herta
{
namespace
{
constexpr std::string_view Usage = R"(HertaCooker cooks a game's content for distribution.

  cook --project=<p.hertaproject> --output=<directory> --mode=loose|package|embedded [--name=<package>] [--shaders=<directory>]
       [--embed-into=<executable>,...] [--signing-key=<file>]
      Stages the project's starting level and every asset it reaches. Loose writes the staged files into the output; package writes
      <name>.hpak there, with the engine shaders from --shaders; embedded builds the same package into each --embed-into executable.
  mod --source=<mod folder> --project=<p.hertaproject> --output=<installed mod folder> [--package] [--signing-key=<file>]
      Cooks a data mod's Content folder for the game into a folder for its Mods directory, its files loose under Files or, with --package,
      in <namespace>.hpak.
  package --input=<directory> --output=<file.hpak> [--signing-key=<file>]
      Packages every file under a staged directory.
  embed --package=<file.hpak> --executable=<file>
      Embeds a package into an executable; on Windows, before code signing.
  keygen --id=<number> --output=<private key file>
      Writes a new signing key and prints its public half for Config/PackagePublicKeys.txt. Keep the private key out of source control.
  verify --package=<file.hpak> [--public-keys=<file>]
      Checks a package's index, signature, and every chunk.)";

void Report(const std::string_view Message)
{
	std::println(stderr, "HertaCooker: {}", Message);
}

[[nodiscard]] std::string_view Find(const std::span<char* const> Arguments, const std::string_view Prefix)
{
	for (const char* const Argument : Arguments)
	{
		if (std::string_view(Argument).starts_with(Prefix))
		{
			return std::string_view(Argument).substr(Prefix.size());
		}
	}

	return {};
}

[[nodiscard]] bool HasFlag(const std::span<char* const> Arguments, const std::string_view Flag)
{
	return std::ranges::any_of(Arguments, [Flag](const char* const Argument)
	{
		return std::string_view(Argument) == Flag;
	});
}

[[nodiscard]] std::filesystem::path PathOf(const std::string_view Text)
{
	return std::filesystem::path(std::u8string(Text.begin(), Text.end()));
}

// The working directory wins, so checkouts can share one installed binary, as HertaEditorCmd does.
[[nodiscard]] std::filesystem::path FindRepositoryRoot()
{
	std::error_code Error;
	for (std::filesystem::path Start : {std::filesystem::current_path(Error), GetExecutablePath().parent_path()})
	{
		for (int Parent = 0; Parent < 8 && !Start.empty(); ++Parent)
		{
			if (std::filesystem::is_directory(Start / "Engine/Content", Error))
			{
				return Start;
			}

			if (Start == Start.parent_path())
			{
				break;
			}

			Start = Start.parent_path();
		}
	}

	return {};
}

[[nodiscard]] std::expected<FPackageBuildOptions, std::string> ReadBuildOptions(const std::span<char* const> Arguments)
{
	FPackageBuildOptions Options;
	if (const std::string_view KeyPath = Find(Arguments, "--signing-key="); !KeyPath.empty())
	{
		const auto Text = ReadDiskFile(PathOf(KeyPath));
		const auto Key = Text ? ParsePackageSigningKey(std::string_view(reinterpret_cast<const char*>(Text->data()), Text->size())) : std::unexpected(FPackageBuildError{Text.error().Message});
		if (!Key)
		{
			return std::unexpected(std::format("Cannot use the signing key {}: {}", KeyPath, Key.error().Message));
		}

		Options.SigningKey = *Key;
	}

	return Options;
}

int Package(const std::span<char* const> Arguments)
{
	const std::string_view Input = Find(Arguments, "--input=");
	const std::string_view Output = Find(Arguments, "--output=");
	const auto Options = ReadBuildOptions(Arguments);
	if (Input.empty() || Output.empty() || !Options)
	{
		Report(Options ? "package needs --input= and --output=" : Options.error());
		return 2;
	}

	auto Inputs = CollectPackageInputs(PathOf(Input));
	const auto Written = Inputs ? WritePackage(PathOf(Output), std::move(*Inputs), *Options) : std::unexpected(Inputs.error());
	if (!Written)
	{
		Report(Written.error().Message);
		return 1;
	}

	std::println("Packaged {} files ({:.1f} MiB) into {} ({:.1f} MiB{})", Written->Entries, static_cast<double>(Written->Bytes) / 1048576., Output, static_cast<double>(Written->PackageBytes) / 1048576., Options->SigningKey ? ", signed" : "");
	return 0;
}

int Embed(const std::span<char* const> Arguments)
{
	const std::string_view PackagePath = Find(Arguments, "--package=");
	const std::string_view Executable = Find(Arguments, "--executable=");
	const auto Bytes = PackagePath.empty() ? std::unexpected(FAssetError{"embed needs --package="}) : ReadDiskFile(PathOf(PackagePath));
	const auto Opened = Bytes ? FContentPackage::OpenMemory(*Bytes) : std::unexpected(Bytes.error());
	if (!Opened || Executable.empty())
	{
		Report(Opened ? "embed needs --executable=" : Opened.error().Message);
		return 2;
	}

	if (auto Embedded = EmbedPackage(PathOf(Executable), *Bytes); !Embedded)
	{
		Report(Embedded.error().Message);
		return 1;
	}

	std::println("Embedded {} into {}", PackagePath, Executable);
	return 0;
}

int Cook(const std::span<char* const> Arguments)
{
	const std::string_view Project = Find(Arguments, "--project=");
	const std::string_view Output = Find(Arguments, "--output=");
	const std::string_view Mode = Find(Arguments, "--mode=");
	const auto Options = ReadBuildOptions(Arguments);
	const bool bLoose = Mode == "loose";
	if (Project.empty() || Output.empty() || (!bLoose && Mode != "package" && Mode != "embedded") || !Options)
	{
		Report(Options ? "cook needs --project=, --output=, and --mode=loose|package|embedded" : Options.error());
		return 2;
	}

	const std::filesystem::path Repository = FindRepositoryRoot();
	if (Repository.empty())
	{
		Report("Cannot find the Herta checkout with Engine/Content to cook from");
		return 1;
	}

	// Package output stages into a scratch folder, removed whatever happens. It sits in the system's temporary folder under a short name,
	// because staged paths are long and deep output folders would pass the Windows path limit.
	const std::filesystem::path OutputRoot = PathOf(Output);
	std::array<std::byte, 6> Nonce{};
	(void)FillSecureRandom(Nonce);
	std::string StageName = "HertaCook-";
	for (const std::byte Byte : Nonce)
	{
		StageName += std::format("{:02x}", static_cast<unsigned>(Byte));
	}

	std::error_code TemporaryError;
	const std::filesystem::path StageRoot = bLoose ? OutputRoot : std::filesystem::temp_directory_path(TemporaryError) / StageName;
	struct FCleanup
	{
		~FCleanup()
		{
			std::error_code Error;
			if (!Path.empty())
			{
				std::filesystem::remove_all(Path, Error);
			}
		}

		std::filesystem::path Path;
	} Cleanup{bLoose ? std::filesystem::path{} : StageRoot};

	const std::string_view Platform = GetPlatformName(GetCurrentPlatform());
	std::filesystem::path Worker = GetExecutablePath().parent_path() / "HertaAssetWorker";
	Worker += GetExecutablePath().extension();
	const auto Staged = StageGameContent({
	    .ProjectPath = PathOf(Project),
	    .OutputRoot = StageRoot,
	    .EngineContentRoot = Repository / "Engine/Content",
	    .EngineShaderRoot = Repository / "Engine/Shaders",
	    .DerivedDataRoot = Repository / "DerivedDataCache" / Platform,
	    .WorkerPath = Worker,
	    .TargetPlatform = std::string(Platform),
	});

	if (!Staged)
	{
		Report(std::format("Staging failed: {}", Staged.error().Message));
		return 1;
	}

	std::println("Staged {} assets ({:.1f} MiB)", Staged->Assets, static_cast<double>(Staged->Bytes) / 1048576.);
	if (bLoose)
	{
		return 0;
	}

	auto Inputs = CollectPackageInputs(StageRoot);
	if (!Inputs)
	{
		Report(Inputs.error().Message);
		return 1;
	}

	// The engine's shaders sit beside the executable, so the package carries them where the game looks for them.
	if (const std::string_view Shaders = Find(Arguments, "--shaders="); !Shaders.empty())
	{
		auto ShaderInputs = CollectPackageInputs(PathOf(Shaders));
		if (!ShaderInputs || ShaderInputs->empty())
		{
			Report(std::format("No cooked shaders in {}", Shaders));
			return 1;
		}

		for (FPackageInput& Shader : *ShaderInputs)
		{
			Shader.Path = "Shaders/" + Shader.Path;
			Inputs->push_back(std::move(Shader));
		}
	}

	const std::string Name = Find(Arguments, "--name=").empty() ? PathOf(Project).stem().string() : std::string(Find(Arguments, "--name="));
	const std::filesystem::path PackagePath = (Mode == "package" ? OutputRoot : StageRoot) / (Name + ".hpak");
	const auto Written = WritePackage(PackagePath, std::move(*Inputs), *Options);
	if (!Written)
	{
		Report(Written.error().Message);
		return 1;
	}

	std::println("Packaged {} files ({:.1f} MiB) into {:.1f} MiB{}", Written->Entries, static_cast<double>(Written->Bytes) / 1048576., static_cast<double>(Written->PackageBytes) / 1048576., Options->SigningKey ? ", signed" : "");
	if (Mode == "package")
	{
		return 0;
	}

	const std::string_view Targets = Find(Arguments, "--embed-into=");
	const auto Bytes = ReadDiskFile(PackagePath);
	if (Targets.empty() || !Bytes)
	{
		Report(Targets.empty() ? "Embedded output needs --embed-into=<executable>" : Bytes.error().Message);
		return 2;
	}

	for (std::size_t Start = 0; Start < Targets.size();)
	{
		const std::size_t End = std::min(Targets.find(',', Start), Targets.size());
		const std::filesystem::path Executable = PathOf(Targets.substr(Start, End - Start));
		Start = End + 1;
		if (auto Embedded = EmbedPackage(Executable, *Bytes); !Embedded)
		{
			Report(Embedded.error().Message);
			return 1;
		}

		std::println("Embedded the package into {}", Executable.generic_string());
	}

	return 0;
}

int Mod(const std::span<char* const> Arguments)
{
	const std::string_view Source = Find(Arguments, "--source=");
	const std::string_view Project = Find(Arguments, "--project=");
	const std::string_view Output = Find(Arguments, "--output=");
	const auto Options = ReadBuildOptions(Arguments);
	const std::filesystem::path Repository = FindRepositoryRoot();
	if (Source.empty() || Project.empty() || Output.empty() || !Options || Repository.empty())
	{
		Report(!Options ? Options.error() : Repository.empty() ? "Cannot find the Herta checkout with Engine/Content to cook from" : "mod needs --source=, --project=, and --output=");
		return 2;
	}

	const std::string_view Platform = GetPlatformName(GetCurrentPlatform());
	std::filesystem::path Worker = GetExecutablePath().parent_path() / "HertaAssetWorker";
	Worker += GetExecutablePath().extension();
	const std::filesystem::path OutputRoot = PathOf(Output);
	const auto Staged = StageModContent({
	    .ModRoot = PathOf(Source),
	    .ProjectPath = PathOf(Project),
	    .OutputRoot = OutputRoot,
	    .EngineContentRoot = Repository / "Engine/Content",
	    .EngineShaderRoot = Repository / "Engine/Shaders",
	    .DerivedDataRoot = Repository / "DerivedDataCache" / Platform,
	    .WorkerPath = Worker,
	    .TargetPlatform = std::string(Platform),
	});

	if (!Staged)
	{
		Report(Staged.error().Message);
		return 1;
	}

	std::println("Cooked mod {} {}.{}.{}: {} files ({:.1f} KiB)", Staged->Manifest.Namespace, Staged->Manifest.Version.Major, Staged->Manifest.Version.Minor, Staged->Manifest.Version.Patch, Staged->Files.size(), static_cast<double>(Staged->Bytes) / 1024.);
	if (!HasFlag(Arguments, "--package"))
	{
		return 0;
	}

	auto Inputs = CollectPackageInputs(OutputRoot / "Files");
	const auto Written = Inputs ? WritePackage(OutputRoot / (Staged->Manifest.Namespace + ".hpak"), std::move(*Inputs), *Options) : std::unexpected(Inputs.error());
	if (!Written)
	{
		Report(Written.error().Message);
		return 1;
	}

	std::error_code Error;
	std::filesystem::remove_all(OutputRoot / "Files", Error);
	std::println("Packaged the mod into {}.hpak", Staged->Manifest.Namespace);
	return 0;
}

int Keygen(const std::span<char* const> Arguments)
{
	const std::string_view IdText = Find(Arguments, "--id=");
	const std::string_view Output = Find(Arguments, "--output=");
	std::uint32_t Id = 0;
	const auto [End, Error] = std::from_chars(IdText.data(), IdText.data() + IdText.size(), Id);
	if (IdText.empty() || Error != std::errc{} || End != IdText.data() + IdText.size() || Output.empty())
	{
		Report("keygen needs --id=<number> and --output=<private key file>");
		return 2;
	}

	std::error_code Exists;
	if (std::filesystem::exists(PathOf(Output), Exists))
	{
		Report(std::format("{} exists; a signing key is never overwritten", Output));
		return 1;
	}

	const auto Key = GeneratePackageSigningKey(Id);
	if (!Key)
	{
		Report(Key.error().Message);
		return 1;
	}

	std::ofstream File(PathOf(Output), std::ios::binary);
	const std::string Text = FormatPackageSigningKey(*Key);
	if (!File || !File.write(Text.data(), static_cast<std::streamsize>(Text.size())))
	{
		Report(std::format("Cannot write {}", Output));
		return 1;
	}

	std::println("{}", FormatPackagePublicKey(GetPackagePublicKey(*Key)));
	return 0;
}

int Verify(const std::span<char* const> Arguments)
{
	const std::string_view PackagePath = Find(Arguments, "--package=");
	std::vector<FPackagePublicKey> Keys;
	if (const std::string_view KeysPath = Find(Arguments, "--public-keys="); !KeysPath.empty())
	{
		const auto Text = ReadDiskFile(PathOf(KeysPath));
		std::string Lines;
		for (const std::byte Byte : Text.value_or(std::vector<std::byte>{}))
		{
			Lines.push_back(static_cast<char>(Byte));
		}

		// Comments start with '#', as in Config/PackagePublicKeys.txt.
		std::string Stripped;
		for (std::size_t Start = 0; Start < Lines.size();)
		{
			const std::size_t End = std::min(Lines.find('\n', Start), Lines.size());
			const std::string_view Line = std::string_view(Lines).substr(Start, End - Start);
			Stripped += std::string(Line.substr(0, Line.find('#'))) + "\n";
			Start = End + 1;
		}

		auto Parsed = Text ? ParsePackagePublicKeys(Stripped) : std::unexpected(Text.error());
		if (!Parsed)
		{
			Report(Parsed.error().Message);
			return 2;
		}

		Keys = std::move(*Parsed);
	}

	const auto Opened = PackagePath.empty() ? std::unexpected(FAssetError{"verify needs --package="}) : FContentPackage::Open(PathOf(PackagePath), {.TrustedKeys = Keys, .bRequireSignature = !Keys.empty()});
	if (!Opened)
	{
		Report(Opened.error().Message);
		return 1;
	}

	std::uint64_t Bytes = 0;
	for (const FPackageEntry& Entry : (*Opened)->GetEntries())
	{
		if (auto Read = (*Opened)->Read(Entry.Path); !Read)
		{
			Report(Read.error().Message);
			return 1;
		}

		Bytes += Entry.Size;
	}

	std::println("{}: {} files, {:.1f} MiB, {}", PackagePath, (*Opened)->GetEntries().size(), static_cast<double>(Bytes) / 1048576., (*Opened)->IsTrusted() ? std::format("signed with trusted key {}", (*Opened)->GetHeader().SigningKey) : (*Opened)->GetHeader().bSigned ? std::string("signed with an unchecked key") : std::string("unsigned"));
	return 0;
}
}
}

int main(const int ArgumentCount, char** const Arguments)
{
	try
	{
		const std::span<char* const> All(Arguments, static_cast<std::size_t>(std::max(ArgumentCount, 0)));
		const std::string_view Command = All.size() > 1 ? All[1] : "";
		const std::span<char* const> Rest = All.size() > 2 ? All.subspan(2) : std::span<char* const>{};
		if (Command == "cook")
		{
			return Herta::Cook(Rest);
		}

		if (Command == "package")
		{
			return Herta::Package(Rest);
		}

		if (Command == "mod")
		{
			return Herta::Mod(Rest);
		}

		if (Command == "embed")
		{
			return Herta::Embed(Rest);
		}

		if (Command == "keygen")
		{
			return Herta::Keygen(Rest);
		}

		if (Command == "verify")
		{
			return Herta::Verify(Rest);
		}

		std::println(stderr, "{}", Herta::Usage);
		return Command.empty() || Command == "--help" ? 0 : 2;
	}
	catch (const std::exception& Exception)
	{
		Herta::Report(Exception.what());
		return 1;
	}
}
