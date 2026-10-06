#include "Herta/Scene/SceneSerialization.h"

#include "Herta/Scene/SceneDescriptors.h"

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

template <std::size_t N> std::expected<std::array<simdjson::dom::element, N>, FSceneError> ReadComponentFields(const simdjson::dom::element Element, const FSceneComponentDescriptor& Descriptor)
{
	std::array<std::string_view, N> Keys;

	for (std::size_t Index = 0; Index < N; ++Index)
	{
		Keys[Index] = Descriptor.Properties[Index].Key;
	}

	return ReadFields(Element, Keys, (std::uint64_t{1} << N) - 1);
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

template <typename T> std::expected<T, FSceneError> ReadNumber(const simdjson::dom::element Element)
{
	double Value = 0;
	if (Element.get_double().get(Value) || !std::isfinite(Value) || Value < -std::numeric_limits<T>::max() || Value > std::numeric_limits<T>::max())
	{
		return SceneError("Invalid scene number");
	}

	return static_cast<T>(Value);
}

std::expected<FSceneEntity, FSceneError> ReadEntity(const simdjson::dom::element Element, const std::uint64_t SchemaVersion)
{
	const auto& TransformDescriptor = GetSceneComponentDescriptor(ESceneComponentType::Transform);
	const auto& MeshDescriptor = GetSceneComponentDescriptor(ESceneComponentType::StaticMesh);
	const auto& BodyDescriptor = GetSceneComponentDescriptor(ESceneComponentType::RigidBody);
	const auto Fields = ReadFields(Element, std::array<std::string_view, 5>{"id", "name", "parent", TransformDescriptor.SerializationKey, "components"}, 0x1f);
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

	const auto Transform = ReadComponentFields<3>((*Fields)[3], TransformDescriptor);
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
		if (Component.key == MeshDescriptor.SerializationKey)
		{
			if ((SeenComponents & 1) != 0)
			{
				return SceneError("Duplicate scene component");
			}

			SeenComponents |= 1;
			const auto Mesh = ReadComponentFields<1>(Component.value, MeshDescriptor);
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
		else if (Component.key == BodyDescriptor.SerializationKey)
		{
			if ((SeenComponents & 2) != 0)
			{
				return SceneError("Duplicate scene component");
			}

			SeenComponents |= 2;

			if (SchemaVersion == 1)
			{
				const auto Body = ReadComponentFields<1>(Component.value, BodyDescriptor);
				if (!Body)
				{
					return std::unexpected(Body.error());
				}

				const auto Type = ReadString((*Body)[0]);
				if (!Type || (*Type != "static" && *Type != "dynamic"))
				{
					return SceneError("Unknown scene body type");
				}

				Entity.BodyType = *Type == "static" ? ESceneBodyType::Static : ESceneBodyType::Dynamic;
				continue;
			}

			const auto Body = ReadComponentFields<7>(Component.value, BodyDescriptor);
			if (!Body)
			{
				return std::unexpected(Body.error());
			}

			const auto Type = ReadString((*Body)[0]);
			if (!Type || (*Type != "static" && *Type != "dynamic"))
			{
				return SceneError("Unknown scene body type");
			}

			const auto MassKg = ReadNumber<float>((*Body)[1]);
			const auto Friction = ReadNumber<float>((*Body)[2]);
			const auto Restitution = ReadNumber<float>((*Body)[3]);
			const auto LinearDamping = ReadNumber<float>((*Body)[4]);
			const auto AngularDamping = ReadNumber<float>((*Body)[5]);
			const auto GravityScale = ReadNumber<float>((*Body)[6]);
			if (!MassKg || !Friction || !Restitution || !LinearDamping || !AngularDamping || !GravityScale)
			{
				return SceneError("Invalid scene rigid body settings");
			}

			Entity.BodyType = *Type == "static" ? ESceneBodyType::Static : ESceneBodyType::Dynamic;
			Entity.BodySettings = {
			    .MassKg = *MassKg,
			    .Friction = *Friction,
			    .Restitution = *Restitution,
			    .LinearDamping = *LinearDamping,
			    .AngularDamping = *AngularDamping,
			    .GravityScale = *GravityScale,
			};
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

void AppendFieldKey(std::string& Output, const std::string_view Prefix, const std::string_view Key)
{
	Output += Prefix;
	AppendString(Output, Key);
	Output += ": ";
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

std::expected<void, FSceneError> ValidateSceneDocument(const FSceneDocument& Document)
{
	if (!Document.Id.IsValid() || !IsValidName(Document.Name) || Document.Entities.size() > MaximumSceneEntities)
	{
		return SceneError("Invalid scene document ID, name, or entity count");
	}

	return ValidateSceneEntities(Document.Entities);
}

std::expected<std::string, FSceneError> SerializeScene(const FSceneDocument& Document)
{
	const auto Validation = ValidateSceneDocument(Document);
	if (!Validation)
	{
		return std::unexpected(Validation.error());
	}

	const auto& TransformDescriptor = GetSceneComponentDescriptor(ESceneComponentType::Transform);
	const auto& MeshDescriptor = GetSceneComponentDescriptor(ESceneComponentType::StaticMesh);
	const auto& BodyDescriptor = GetSceneComponentDescriptor(ESceneComponentType::RigidBody);
	std::vector<const FSceneEntity*> Entities;
	Entities.reserve(Document.Entities.size());

	for (const auto& Entity : Document.Entities)
	{
		Entities.push_back(&Entity);
	}

	std::ranges::sort(Entities, [](const FSceneEntity* Left, const FSceneEntity* Right)
	{
		return Left->Id < Right->Id;
	});

	std::string Output = "{\n  \"format\": \"HertaScene\",\n  \"formatVersion\": 1,\n  \"engineSchemaVersion\": 2,\n  \"id\": ";
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

		AppendFieldKey(Output, ",\n      ", TransformDescriptor.SerializationKey);
		AppendFieldKey(Output, "{\n        ", TransformDescriptor.Properties[0].Key);
		const auto& Position = Entity.Transform.Translation.Meters;
		AppendNumbers(Output, std::array{Position.X, Position.Y, Position.Z});
		AppendFieldKey(Output, ",\n        ", TransformDescriptor.Properties[1].Key);
		AppendNumbers(Output, Entity.Transform.Rotation.ToXYZW());
		AppendFieldKey(Output, ",\n        ", TransformDescriptor.Properties[2].Key);
		const auto& Scale = Entity.Transform.Scale;
		AppendNumbers(Output, std::array{Scale.X, Scale.Y, Scale.Z});
		Output += "\n      },\n      \"components\": {";

		if (Entity.Mesh)
		{
			AppendFieldKey(Output, "\n        ", MeshDescriptor.SerializationKey);
			AppendFieldKey(Output, "{", MeshDescriptor.Properties[0].Key);
			AppendString(Output, Entity.Mesh->Asset.ToString());
			Output += '}';
		}

		if (Entity.BodyType != ESceneBodyType::None)
		{
			Output += Entity.Mesh ? ",\n" : "\n";
			AppendFieldKey(Output, "        ", BodyDescriptor.SerializationKey);
			AppendFieldKey(Output, "{\n          ", BodyDescriptor.Properties[0].Key);
			AppendString(Output, Entity.BodyType == ESceneBodyType::Static ? "static" : "dynamic");
			AppendFieldKey(Output, ",\n          ", BodyDescriptor.Properties[1].Key);
			AppendNumber(Output, Entity.BodySettings.MassKg);
			AppendFieldKey(Output, ",\n          ", BodyDescriptor.Properties[2].Key);
			AppendNumber(Output, Entity.BodySettings.Friction);
			AppendFieldKey(Output, ",\n          ", BodyDescriptor.Properties[3].Key);
			AppendNumber(Output, Entity.BodySettings.Restitution);
			AppendFieldKey(Output, ",\n          ", BodyDescriptor.Properties[4].Key);
			AppendNumber(Output, Entity.BodySettings.LinearDamping);
			AppendFieldKey(Output, ",\n          ", BodyDescriptor.Properties[5].Key);
			AppendNumber(Output, Entity.BodySettings.AngularDamping);
			AppendFieldKey(Output, ",\n          ", BodyDescriptor.Properties[6].Key);
			AppendNumber(Output, Entity.BodySettings.GravityScale);
			Output += "\n        }";
		}

		Output += Entity.Mesh || Entity.BodyType != ESceneBodyType::None ? "\n      }\n    }" : "}\n    }";
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

	const auto Fields = ReadFields(Root, std::array<std::string_view, 6>{"format", "formatVersion", "engineSchemaVersion", "id", "name", "entities"}, 0x3f);
	if (!Fields)
	{
		return std::unexpected(Fields.error());
	}

	const auto Format = ReadString((*Fields)[0]);
	std::uint64_t FormatVersion = 0;
	std::uint64_t SchemaVersion = 0;
	if (!Format || *Format != "HertaScene" || (*Fields)[1].get_uint64().get(FormatVersion) || (*Fields)[2].get_uint64().get(SchemaVersion))
	{
		return SceneError("Invalid scene format header");
	}

	if (FormatVersion != 1 || (SchemaVersion != 1 && SchemaVersion != 2))
	{
		return SceneError("Unsupported scene format or engine schema version; supported format is 1 and engine schemas are 1 through 2");
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
		auto Entity = ReadEntity(Element, SchemaVersion);
		if (!Entity)
		{
			return std::unexpected(Entity.error());
		}

		Document.Entities.push_back(std::move(*Entity));
	}

	const auto Validation = ValidateSceneDocument(Document);
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
