#include "Herta/Scene/SceneSerialization.h"

#include <simdjson.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <exception>
#include <fstream>
#include <limits>
#include <span>
#include <system_error>

#ifdef _WIN32
	#include <Windows.h>
#else
	#include <fcntl.h>
	#include <unistd.h>

	#include <cerrno>
#endif

namespace Herta
{
namespace
{
constexpr std::size_t MaximumSceneBytes = 64 * 1024 * 1024;
constexpr std::size_t MaximumSceneEntities = 1'000'000;
constexpr std::size_t MaximumNameBytes = 1024;

std::unexpected<FSceneError> SceneError(const std::string_view Message)
{
	return std::unexpected(FSceneError{std::string(Message)});
}

bool IsValidName(const std::string_view Name)
{
	if (Name.size() > MaximumNameBytes || !simdjson::validate_utf8(Name.data(), Name.size()))
	{
		return false;
	}

	for (std::size_t Index = 0; Index < Name.size(); ++Index)
	{
		const auto Byte = static_cast<unsigned char>(Name[Index]);
		if (Byte < 0x20 || Byte == 0x7f || (Byte == 0xc2 && Index + 1 < Name.size() && static_cast<unsigned char>(Name[Index + 1]) <= 0x9f))
		{
			return false;
		}
	}

	return true;
}

template <std::size_t N> std::expected<std::array<simdjson::dom::element, N>, FSceneError> ReadFields(const simdjson::dom::element Element, const std::array<std::string_view, N>& Names, const std::uint64_t Required)
{
	simdjson::dom::object Object;
	if (Element.get_object().get(Object))
	{
		return SceneError("Expected a scene JSON object");
	}

	std::array<simdjson::dom::element, N> Values;
	std::uint64_t Seen = 0;

	for (const auto Field : Object)
	{
		const auto Iterator = std::ranges::find(Names, Field.key);
		if (Iterator == Names.end())
		{
			return SceneError("Unknown scene JSON field");
		}

		const auto Index = static_cast<std::size_t>(Iterator - Names.begin());
		const std::uint64_t Bit = std::uint64_t{1} << Index;
		if ((Seen & Bit) != 0)
		{
			return SceneError("Duplicate scene JSON field");
		}

		Seen |= Bit;
		Values[Index] = Field.value;
	}

	if ((Seen & Required) != Required)
	{
		return SceneError("Missing required scene JSON field");
	}

	return Values;
}

std::expected<std::string_view, FSceneError> ReadString(const simdjson::dom::element Element)
{
	std::string_view Text;
	if (Element.get_string().get(Text))
	{
		return SceneError("Expected a scene JSON string");
	}

	return Text;
}

template <typename TId> std::expected<TId, FSceneError> ReadId(const simdjson::dom::element Element)
{
	const auto Text = ReadString(Element);
	if (!Text)
	{
		return std::unexpected(Text.error());
	}

	const auto Id = TId::Parse(*Text);
	if (!Id || !Id->IsValid())
	{
		return SceneError("Invalid stable scene ID");
	}

	return *Id;
}

template <typename T, std::size_t N> std::expected<std::array<T, N>, FSceneError> ReadNumbers(const simdjson::dom::element Element)
{
	simdjson::dom::array Array;
	if (Element.get_array().get(Array) || Array.size() != N)
	{
		return SceneError("Invalid scene transform array");
	}

	std::array<T, N> Values;
	std::size_t Index = 0;

	for (const auto Number : Array)
	{
		double Value = 0;
		if (Number.get_double().get(Value) || !std::isfinite(Value) || Value < -std::numeric_limits<T>::max() || Value > std::numeric_limits<T>::max())
		{
			return SceneError("Invalid scene transform number");
		}

		Values[Index++] = static_cast<T>(Value);
	}

	return Values;
}

std::expected<FSceneEntity, FSceneError> ReadEntity(const simdjson::dom::element Element)
{
	const auto Fields = ReadFields(Element, std::array<std::string_view, 5>{"id", "name", "parent", "transform", "components"}, 0x1f);
	if (!Fields)
	{
		return std::unexpected(Fields.error());
	}

	const auto Id = ReadId<FObjectId>((*Fields)[0]);
	const auto Name = ReadString((*Fields)[1]);
	if (!Id || !Name || !IsValidName(*Name))
	{
		return SceneError("Invalid scene entity ID or name");
	}

	FSceneEntity Entity{.Id = *Id, .Name = std::string(*Name)};
	if (!(*Fields)[2].is_null())
	{
		const auto Parent = ReadId<FObjectId>((*Fields)[2]);
		if (!Parent)
		{
			return std::unexpected(Parent.error());
		}

		Entity.Parent = *Parent;
	}

	const auto Transform = ReadFields((*Fields)[3], std::array<std::string_view, 3>{"translation", "rotation", "scale"}, 0x7);
	if (!Transform)
	{
		return std::unexpected(Transform.error());
	}

	const auto Translation = ReadNumbers<double, 3>((*Transform)[0]);
	const auto Rotation = ReadNumbers<float, 4>((*Transform)[1]);
	const auto Scale = ReadNumbers<float, 3>((*Transform)[2]);
	if (!Translation || !Rotation || !Scale)
	{
		return SceneError("Invalid scene transform");
	}

	Entity.Transform = FSceneTransform{
	    .Translation = FWorldPosition{(*Translation)[0], (*Translation)[1], (*Translation)[2]},
	    .Rotation = FQuaternion{(*Rotation)[0], (*Rotation)[1], (*Rotation)[2], (*Rotation)[3]},
	    .Scale = FVector3{(*Scale)[0], (*Scale)[1], (*Scale)[2]},
	};

	simdjson::dom::object Components;
	if ((*Fields)[4].get_object().get(Components))
	{
		return SceneError("Expected scene components object");
	}

	std::uint32_t SeenComponents = 0;

	for (const auto Component : Components)
	{
		if (Component.key == "staticMesh")
		{
			if ((SeenComponents & 1) != 0)
			{
				return SceneError("Duplicate scene component");
			}

			SeenComponents |= 1;
			const auto Mesh = ReadFields(Component.value, std::array<std::string_view, 1>{"asset"}, 1);
			if (!Mesh)
			{
				return std::unexpected(Mesh.error());
			}

			const auto Asset = ReadId<FAssetId>((*Mesh)[0]);
			if (!Asset)
			{
				return std::unexpected(Asset.error());
			}

			Entity.Mesh = FStaticMeshComponent{.Asset = *Asset};
		}
		else if (Component.key == "body")
		{
			if ((SeenComponents & 2) != 0)
			{
				return SceneError("Duplicate scene component");
			}

			SeenComponents |= 2;
			const auto Body = ReadFields(Component.value, std::array<std::string_view, 1>{"motion"}, 1);
			if (!Body)
			{
				return std::unexpected(Body.error());
			}

			const auto Motion = ReadString((*Body)[0]);
			if (!Motion || (*Motion != "static" && *Motion != "dynamic"))
			{
				return SceneError("Unknown scene body motion");
			}

			Entity.BodyMotion = *Motion == "static" ? ESceneBodyMotion::Static : ESceneBodyMotion::Dynamic;
		}
		else
		{
			return SceneError("Unknown scene component");
		}
	}

	return Entity;
}

void AppendString(std::string& Output, const std::string_view Text)
{
	Output += '"';

	for (const char Character : Text)
	{
		if (Character == '"' || Character == '\\')
		{
			Output += '\\';
		}

		Output += Character;
	}

	Output += '"';
}

template <typename T> void AppendNumber(std::string& Output, const T Value)
{
	std::array<char, 64> Buffer;
	const auto Result = std::to_chars(Buffer.data(), Buffer.data() + Buffer.size(), Value == 0 ? T{0} : Value, std::chars_format::general);
	Output.append(Buffer.data(), Result.ptr);
}

template <typename T, std::size_t N> void AppendNumbers(std::string& Output, const std::array<T, N>& Values)
{
	Output += '[';

	for (std::size_t Index = 0; Index < N; ++Index)
	{
		if (Index != 0)
		{
			Output += ", ";
		}

		AppendNumber(Output, Values[Index]);
	}

	Output += ']';
}

std::expected<void, FSceneError> WriteTemporaryFile(const std::filesystem::path& Path, const std::string_view Text)
{
#ifdef _WIN32
	const HANDLE File = CreateFileW(Path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (File == INVALID_HANDLE_VALUE)
	{
		return SceneError("Cannot create scene temporary file");
	}

	DWORD Written = 0;
	const bool bWritten = WriteFile(File, Text.data(), static_cast<DWORD>(Text.size()), &Written, nullptr) != FALSE && Written == Text.size();
	const bool bFlushed = bWritten && FlushFileBuffers(File) != FALSE;
	const bool bClosed = CloseHandle(File) != FALSE;
#else
	const int File = open(Path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
	if (File < 0)
	{
		return SceneError("Cannot create scene temporary file");
	}

	std::size_t Written = 0;

	while (Written < Text.size())
	{
		const ssize_t Result = write(File, Text.data() + Written, Text.size() - Written);
		if (Result < 0 && errno == EINTR)
		{
			continue;
		}

		if (Result <= 0)
		{
			break;
		}

		Written += static_cast<std::size_t>(Result);
	}

	const bool bWritten = Written == Text.size();
	const bool bFlushed = bWritten && fsync(File) == 0;
	const bool bClosed = close(File) == 0;
#endif
	if (!bWritten || !bFlushed || !bClosed)
	{
		std::error_code Error;
		std::filesystem::remove(Path, Error);
		return SceneError("Cannot write scene temporary file");
	}

	return {};
}
}

std::expected<std::string, FSceneError> SerializeScene(const FSceneDocument& Document)
{
	if (!Document.Id.IsValid() || !IsValidName(Document.Name) || Document.Entities.size() > MaximumSceneEntities)
	{
		return SceneError("Invalid scene document ID, name, or entity count");
	}

	const auto Validation = ValidateSceneEntities(Document.Entities);
	if (!Validation)
	{
		return std::unexpected(Validation.error());
	}

	std::vector<const FSceneEntity*> Entities;
	Entities.reserve(Document.Entities.size());

	for (const auto& Entity : Document.Entities)
	{
		if (!IsValidName(Entity.Name))
		{
			return SceneError("Invalid scene entity name");
		}

		Entities.push_back(&Entity);
	}

	std::ranges::sort(Entities, [](const FSceneEntity* Left, const FSceneEntity* Right)
	{
		return Left->Id < Right->Id;
	});

	std::string Output = "{\n  \"magic\": \"HertaScene\",\n  \"formatVersion\": 1,\n  \"engineSchemaVersion\": 1,\n  \"id\": ";
	AppendString(Output, Document.Id.ToString());
	Output += ",\n  \"name\": ";
	AppendString(Output, Document.Name);
	Output += ",\n  \"entities\": [";

	for (std::size_t Index = 0; Index < Entities.size(); ++Index)
	{
		const FSceneEntity& Entity = *Entities[Index];
		Output += Index == 0 ? "\n" : ",\n";
		Output += "    {\n      \"id\": ";
		AppendString(Output, Entity.Id.ToString());
		Output += ",\n      \"name\": ";
		AppendString(Output, Entity.Name);
		Output += ",\n      \"parent\": ";

		if (Entity.Parent.IsValid())
		{
			AppendString(Output, Entity.Parent.ToString());
		}
		else
		{
			Output += "null";
		}

		Output += ",\n      \"transform\": {\n        \"translation\": ";
		const auto& Position = Entity.Transform.Translation.Meters;
		AppendNumbers(Output, std::array{Position.X, Position.Y, Position.Z});
		Output += ",\n        \"rotation\": ";
		AppendNumbers(Output, Entity.Transform.Rotation.ToXYZW());
		Output += ",\n        \"scale\": ";
		const auto& Scale = Entity.Transform.Scale;
		AppendNumbers(Output, std::array{Scale.X, Scale.Y, Scale.Z});
		Output += "\n      },\n      \"components\": {";

		if (Entity.Mesh)
		{
			Output += "\n        \"staticMesh\": {\"asset\": ";
			AppendString(Output, Entity.Mesh->Asset.ToString());
			Output += '}';
		}

		if (Entity.BodyMotion != ESceneBodyMotion::None)
		{
			Output += Entity.Mesh ? ",\n" : "\n";
			Output += "        \"body\": {\"motion\": ";
			AppendString(Output, Entity.BodyMotion == ESceneBodyMotion::Static ? "static" : "dynamic");
			Output += '}';
		}

		Output += Entity.Mesh || Entity.BodyMotion != ESceneBodyMotion::None ? "\n      }\n    }" : "}\n    }";
		if (Output.size() > MaximumSceneBytes)
		{
			return SceneError("Scene exceeds the 64 MiB limit");
		}
	}

	Output += Entities.empty() ? "]\n}\n" : "\n  ]\n}\n";
	if (Output.size() > MaximumSceneBytes)
	{
		return SceneError("Scene exceeds the 64 MiB limit");
	}

	return Output;
}

std::expected<FSceneDocument, FSceneError> ParseScene(const std::string_view Text)
{
	if (Text.size() > MaximumSceneBytes)
	{
		return SceneError("Scene exceeds the 64 MiB limit");
	}

	simdjson::dom::parser Parser(MaximumSceneBytes);
	simdjson::dom::element Root;
	const auto ParseError = Parser.parse(Text.data(), Text.size()).get(Root);
	if (ParseError == simdjson::MEMALLOC)
	{
		std::terminate();
	}

	if (ParseError)
	{
		return SceneError("Malformed scene JSON or invalid UTF-8");
	}

	const auto Fields = ReadFields(Root, std::array<std::string_view, 6>{"magic", "formatVersion", "engineSchemaVersion", "id", "name", "entities"}, 0x3f);
	if (!Fields)
	{
		return std::unexpected(Fields.error());
	}

	const auto Magic = ReadString((*Fields)[0]);
	std::uint64_t FormatVersion = 0;
	std::uint64_t SchemaVersion = 0;
	if (!Magic || *Magic != "HertaScene" || (*Fields)[1].get_uint64().get(FormatVersion) || (*Fields)[2].get_uint64().get(SchemaVersion))
	{
		return SceneError("Invalid scene format header");
	}

	if (FormatVersion != 1 || SchemaVersion != 1)
	{
		return SceneError("Unsupported scene format or engine schema version; supported versions are 1 and 1");
	}

	const auto Id = ReadId<FObjectId>((*Fields)[3]);
	const auto Name = ReadString((*Fields)[4]);
	if (!Id || !Name || !IsValidName(*Name))
	{
		return SceneError("Invalid scene document ID or name");
	}

	simdjson::dom::array Entities;
	if ((*Fields)[5].get_array().get(Entities) || Entities.size() > MaximumSceneEntities)
	{
		return SceneError("Invalid scene entities array or entity count");
	}

	FSceneDocument Document{.Id = *Id, .Name = std::string(*Name)};
	Document.Entities.reserve(Entities.size());

	for (const auto Element : Entities)
	{
		auto Entity = ReadEntity(Element);
		if (!Entity)
		{
			return std::unexpected(Entity.error());
		}

		Document.Entities.push_back(std::move(*Entity));
	}

	const auto Validation = ValidateSceneEntities(Document.Entities);
	if (!Validation)
	{
		return std::unexpected(Validation.error());
	}

	return Document;
}

std::expected<FSceneDocument, FSceneError> LoadScene(const std::filesystem::path& Path)
{
	std::error_code Error;
	const std::uintmax_t Size = std::filesystem::file_size(Path, Error);
	if (Error || Size > MaximumSceneBytes)
	{
		return SceneError("Cannot read scene file or scene exceeds the 64 MiB limit");
	}

	std::ifstream Stream(Path, std::ios::binary);
	std::string Text(static_cast<std::size_t>(Size), '\0');
	Stream.read(Text.data(), static_cast<std::streamsize>(Text.size()));
	if (!Stream)
	{
		return SceneError("Cannot read complete scene file");
	}

	if (Stream.peek() != std::char_traits<char>::eof() || Stream.bad())
	{
		return SceneError("Scene file changed during read or could not be read completely");
	}

	return ParseScene(Text);
}

std::expected<void, FSceneError> SaveScene(const std::filesystem::path& Path, const FSceneDocument& Document)
{
	const auto Text = SerializeScene(Document);
	if (!Text)
	{
		return std::unexpected(Text.error());
	}

	std::filesystem::path TemporaryPath = Path;
	TemporaryPath += "." + FObjectId::Generate().ToString() + ".tmp";
	const auto Written = WriteTemporaryFile(TemporaryPath, *Text);
	std::error_code Error;
	if (!Written)
	{
		return std::unexpected(Written.error());
	}

#ifdef _WIN32
	// std::filesystem::rename does not replace an existing destination on Windows.
	const bool bPublished = MoveFileExW(TemporaryPath.c_str(), Path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
	std::filesystem::rename(TemporaryPath, Path, Error);
	const bool bPublished = !Error;
#endif
	if (!bPublished)
	{
		std::filesystem::remove(TemporaryPath, Error);
		return SceneError("Cannot atomically replace scene file");
	}

	return {};
}
}
