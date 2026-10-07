#include "Herta/Level/LevelSerialization.h"

#include "Herta/Level/LevelDescriptors.h"

#include <simdjson.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <span>
#include <system_error>
#include <type_traits>

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
constexpr std::size_t MaximumLevelBytes = 64 * 1024 * 1024;
constexpr std::size_t MaximumLevelEntities = 1'000'000;
constexpr std::size_t MaximumNameBytes = 1024;

std::unexpected<FLevelError> LevelError(const std::string_view Message)
{
	return std::unexpected(FLevelError{std::string(Message)});
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

template <std::size_t N> std::expected<std::array<simdjson::dom::element, N>, FLevelError> ReadFields(const simdjson::dom::element Element, const std::array<std::string_view, N>& Names, const std::uint64_t Required)
{
	simdjson::dom::object Object;
	if (Element.get_object().get(Object))
	{
		return LevelError("Expected a level JSON object");
	}

	std::array<simdjson::dom::element, N> Values;
	std::uint64_t Seen = 0;

	for (const auto Field : Object)
	{
		const auto Iterator = std::ranges::find(Names, Field.key);
		if (Iterator == Names.end())
		{
			return LevelError("Unknown level JSON field");
		}

		const auto Index = static_cast<std::size_t>(Iterator - Names.begin());
		const std::uint64_t Bit = std::uint64_t{1} << Index;
		if ((Seen & Bit) != 0)
		{
			return LevelError("Duplicate level JSON field");
		}

		Seen |= Bit;
		Values[Index] = Field.value;
	}

	if ((Seen & Required) != Required)
	{
		return LevelError("Missing required level JSON field");
	}

	return Values;
}

template <std::size_t N> std::expected<std::array<simdjson::dom::element, N>, FLevelError> ReadComponentFields(const simdjson::dom::element Element, const FLevelComponentDescriptor& Descriptor)
{
	std::array<std::string_view, N> Keys;

	for (std::size_t Index = 0; Index < N; ++Index)
	{
		Keys[Index] = Descriptor.Properties[Index].Key;
	}

	return ReadFields(Element, Keys, (std::uint64_t{1} << N) - 1);
}

std::expected<std::string_view, FLevelError> ReadString(const simdjson::dom::element Element)
{
	std::string_view Text;
	if (Element.get_string().get(Text))
	{
		return LevelError("Expected a level JSON string");
	}

	return Text;
}

template <typename TId> std::expected<TId, FLevelError> ReadId(const simdjson::dom::element Element)
{
	const auto Text = ReadString(Element);
	if (!Text)
	{
		return std::unexpected(Text.error());
	}

	const auto Id = TId::Parse(*Text);
	if (!Id || !Id->IsValid())
	{
		return LevelError("Invalid stable level ID");
	}

	return *Id;
}

template <typename T, std::size_t N> std::expected<std::array<T, N>, FLevelError> ReadNumbers(const simdjson::dom::element Element)
{
	simdjson::dom::array Array;
	if (Element.get_array().get(Array) || Array.size() != N)
	{
		return LevelError("Invalid level transform array");
	}

	std::array<T, N> Values;
	std::size_t Index = 0;

	for (const auto Number : Array)
	{
		double Value = 0;
		if (Number.get_double().get(Value) || !std::isfinite(Value) || Value < -std::numeric_limits<T>::max() || Value > std::numeric_limits<T>::max())
		{
			return LevelError("Invalid level transform number");
		}

		Values[Index++] = static_cast<T>(Value);
	}

	return Values;
}

template <typename T> std::expected<T, FLevelError> ReadNumber(const simdjson::dom::element Element)
{
	double Value = 0;
	if (Element.get_double().get(Value) || !std::isfinite(Value) || Value < -std::numeric_limits<T>::max() || Value > std::numeric_limits<T>::max())
	{
		return LevelError("Invalid level number");
	}

	return static_cast<T>(Value);
}

std::expected<bool, FLevelError> ReadBoolean(const simdjson::dom::element Element)
{
	bool Value = false;
	if (Element.get_bool().get(Value))
	{
		return LevelError("Expected a level JSON boolean");
	}

	return Value;
}

template <typename TId> std::expected<TId, FLevelError> ReadOptionalId(const simdjson::dom::element Element)
{
	return Element.is_null() ? std::expected<TId, FLevelError>{TId{}} : ReadId<TId>(Element);
}

std::expected<FLightComponent, FLevelError> ReadLight(const simdjson::dom::element Element)
{
	const auto Fields = ReadComponentFields<17>(Element, GetLevelComponentDescriptor(ELevelComponentType::Light));
	if (!Fields)
	{
		return std::unexpected(Fields.error());
	}

	const auto Type = ReadString((*Fields)[0]);
	constexpr std::array<std::string_view, 5> Types{"directional", "sky", "point", "spot", "rect"};
	const auto Color = ReadNumbers<float, 3>((*Fields)[1]);
	const auto Intensity = ReadNumber<float>((*Fields)[2]);
	const auto UseTemperature = ReadBoolean((*Fields)[3]);
	const auto Temperature = ReadNumber<float>((*Fields)[4]);
	const auto CastShadows = ReadBoolean((*Fields)[5]);
	const auto Bias = ReadNumber<float>((*Fields)[6]);
	const auto NormalBias = ReadNumber<float>((*Fields)[7]);
	const auto Range = ReadNumber<float>((*Fields)[8]);
	const auto Inner = ReadNumber<float>((*Fields)[9]);
	const auto Outer = ReadNumber<float>((*Fields)[10]);
	const auto Width = ReadNumber<float>((*Fields)[11]);
	const auto Height = ReadNumber<float>((*Fields)[12]);
	const auto Environment = ReadOptionalId<FAssetId>((*Fields)[13]);
	const auto Ambient = ReadNumber<float>((*Fields)[14]);
	const auto Visible = ReadBoolean((*Fields)[15]);
	const auto Enabled = ReadBoolean((*Fields)[16]);
	if (!Type || std::ranges::find(Types, *Type) == Types.end() || !Color || !Intensity || !UseTemperature || !Temperature || !CastShadows || !Bias || !NormalBias
	    || !Range || !Inner || !Outer || !Width || !Height || !Environment || !Ambient || !Visible || !Enabled)
	{
		return LevelError("Invalid light component field");
	}

	return FLightComponent{
	    .Type = static_cast<ELightType>(std::ranges::find(Types, *Type) - Types.begin()),
	    .Color = {(*Color)[0], (*Color)[1], (*Color)[2]},
	    .Intensity = *Intensity,
	    .bUseTemperature = *UseTemperature,
	    .TemperatureKelvin = *Temperature,
	    .bCastShadows = *CastShadows,
	    .ShadowBias = *Bias,
	    .ShadowNormalBias = *NormalBias,
	    .Range = *Range,
	    .InnerConeAngle = *Inner,
	    .OuterConeAngle = *Outer,
	    .Width = *Width,
	    .Height = *Height,
	    .Environment = *Environment,
	    .AmbientStrength = *Ambient,
	    .bEnvironmentVisible = *Visible,
	    .bEnabled = *Enabled,
	};
}

std::expected<FSkyAtmosphereComponent, FLevelError> ReadAtmosphere(const simdjson::dom::element Element)
{
	const auto Fields = ReadComponentFields<7>(Element, GetLevelComponentDescriptor(ELevelComponentType::SkyAtmosphere));
	if (!Fields)
	{
		return std::unexpected(Fields.error());
	}

	const auto Sun = ReadOptionalId<FObjectId>((*Fields)[0]);
	const auto Rayleigh = ReadNumber<float>((*Fields)[1]);
	const auto Mie = ReadNumber<float>((*Fields)[2]);
	const auto Anisotropy = ReadNumber<float>((*Fields)[3]);
	const auto Radius = ReadNumber<float>((*Fields)[4]);
	const auto Height = ReadNumber<float>((*Fields)[5]);
	const auto Enabled = ReadBoolean((*Fields)[6]);
	if (!Sun || !Rayleigh || !Mie || !Anisotropy || !Radius || !Height || !Enabled)
	{
		return LevelError("Invalid atmosphere component field");
	}

	return FSkyAtmosphereComponent{.Sun = *Sun, .RayleighScattering = *Rayleigh, .MieScattering = *Mie, .MieAnisotropy = *Anisotropy, .PlanetRadius = *Radius, .AtmosphereHeight = *Height, .bEnabled = *Enabled};
}

std::expected<FHeightFogComponent, FLevelError> ReadFog(const simdjson::dom::element Element)
{
	const auto Fields = ReadComponentFields<8>(Element, GetLevelComponentDescriptor(ELevelComponentType::HeightFog));
	if (!Fields)
	{
		return std::unexpected(Fields.error());
	}

	const auto Density = ReadNumber<float>((*Fields)[0]);
	const auto Falloff = ReadNumber<float>((*Fields)[1]);
	const auto Albedo = ReadNumbers<float, 3>((*Fields)[2]);
	const auto Anisotropy = ReadNumber<float>((*Fields)[3]);
	const auto Distance = ReadNumber<float>((*Fields)[4]);
	const auto Quality = ReadString((*Fields)[5]);
	constexpr std::array<std::string_view, 3> Qualities{"low", "medium", "high"};
	const auto Enabled = ReadBoolean((*Fields)[6]);
	const auto Volumetric = ReadBoolean((*Fields)[7]);
	if (!Density || !Falloff || !Albedo || !Anisotropy || !Distance || !Quality || std::ranges::find(Qualities, *Quality) == Qualities.end() || !Enabled || !Volumetric)
	{
		return LevelError("Invalid height fog component field");
	}

	return FHeightFogComponent{
	    .Density = *Density,
	    .HeightFalloff = *Falloff,
	    .Albedo = {(*Albedo)[0], (*Albedo)[1], (*Albedo)[2]},
	    .Anisotropy = *Anisotropy,
	    .MaxDistance = *Distance,
	    .Quality = static_cast<EFogQuality>(std::ranges::find(Qualities, *Quality) - Qualities.begin()),
	    .bEnabled = *Enabled,
	    .bVolumetric = *Volumetric,
	};
}

constexpr std::array<std::string_view, 3> SoftBodyShapes{"rope", "cloth", "ball"};

std::expected<FSoftBodyComponent, FLevelError> ReadSoftBody(const simdjson::dom::element Element)
{
	const auto Fields = ReadComponentFields<11>(Element, GetLevelComponentDescriptor(ELevelComponentType::SoftBody));
	if (!Fields)
	{
		return std::unexpected(Fields.error());
	}

	const auto Shape = ReadString((*Fields)[0]);
	const auto Length = ReadNumber<float>((*Fields)[1]);
	const auto Height = ReadNumber<float>((*Fields)[2]);
	const auto Thickness = ReadNumber<float>((*Fields)[3]);
	const auto Mass = ReadNumber<float>((*Fields)[4]);
	const auto Stiffness = ReadNumber<float>((*Fields)[5]);
	const auto Pressure = ReadNumber<float>((*Fields)[6]);
	const auto Friction = ReadNumber<float>((*Fields)[7]);
	const auto Pinned = ReadBoolean((*Fields)[8]);
	const auto Material = ReadOptionalId<FAssetId>((*Fields)[9]);
	const auto Attachment = ReadOptionalId<FObjectId>((*Fields)[10]);
	if (!Shape || std::ranges::find(SoftBodyShapes, *Shape) == SoftBodyShapes.end() || !Length || !Height || !Thickness || !Mass || !Stiffness || !Pressure || !Friction || !Pinned || !Material || !Attachment)
	{
		return LevelError("Invalid soft body component field");
	}

	return FSoftBodyComponent{
	    .Shape = static_cast<ESoftBodyShape>(std::ranges::find(SoftBodyShapes, *Shape) - SoftBodyShapes.begin()),
	    .Length = *Length,
	    .Height = *Height,
	    .Thickness = *Thickness,
	    .MassKg = *Mass,
	    .Stiffness = *Stiffness,
	    .Pressure = *Pressure,
	    .Friction = *Friction,
	    .bPinned = *Pinned,
	    .Material = *Material,
	    .Attachment = *Attachment,
	};
}

std::expected<FLevelEntity, FLevelError> ReadEntity(const simdjson::dom::element Element, const std::uint64_t SchemaVersion)
{
	const auto& TransformDescriptor = GetLevelComponentDescriptor(ELevelComponentType::Transform);
	const auto& MeshDescriptor = GetLevelComponentDescriptor(ELevelComponentType::StaticMesh);
	const auto& BodyDescriptor = GetLevelComponentDescriptor(ELevelComponentType::RigidBody);
	const auto Fields = ReadFields(Element, std::array<std::string_view, 5>{"id", "name", "parent", TransformDescriptor.SerializationKey, "components"}, 0x1f);
	if (!Fields)
	{
		return std::unexpected(Fields.error());
	}

	const auto Id = ReadId<FObjectId>((*Fields)[0]);
	const auto Name = ReadString((*Fields)[1]);
	if (!Id || !Name || !IsValidName(*Name))
	{
		return LevelError("Invalid level entity ID or name");
	}

	FLevelEntity Entity{.Id = *Id, .Name = std::string(*Name)};
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
		return LevelError("Invalid level transform");
	}

	Entity.Transform = FLevelTransform{
	    .Translation = FWorldPosition{(*Translation)[0], (*Translation)[1], (*Translation)[2]},
	    .Rotation = FQuaternion{(*Rotation)[0], (*Rotation)[1], (*Rotation)[2], (*Rotation)[3]},
	    .Scale = FVector3{(*Scale)[0], (*Scale)[1], (*Scale)[2]},
	};

	simdjson::dom::object Components;
	if ((*Fields)[4].get_object().get(Components))
	{
		return LevelError("Expected level components object");
	}

	std::uint32_t SeenComponents = 0;

	for (const auto Component : Components)
	{
		if (Component.key == MeshDescriptor.SerializationKey)
		{
			if ((SeenComponents & 1) != 0)
			{
				return LevelError("Duplicate level component");
			}

			SeenComponents |= 1;

			if (SchemaVersion < 4 && Component.value["materials"].error() == simdjson::SUCCESS)
			{
				return LevelError("Material slot overrides require engine schema 4");
			}

			const auto Mesh = ReadFields(Component.value, std::array<std::string_view, 2>{MeshDescriptor.Properties[0].Key, MeshDescriptor.Properties[1].Key}, SchemaVersion >= 4 ? 3 : 1);
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

			if (SchemaVersion >= 4)
			{
				simdjson::dom::array Materials;
				if ((*Mesh)[1].get_array().get(Materials) || Materials.size() > 256)
				{
					return LevelError("Invalid mesh material slots array");
				}

				for (const auto Material : Materials)
				{
					const auto MaterialId = ReadOptionalId<FAssetId>(Material);
					if (!MaterialId)
					{
						return std::unexpected(MaterialId.error());
					}

					Entity.Mesh->Materials.push_back(*MaterialId);
				}
			}
		}
		else if (Component.key == BodyDescriptor.SerializationKey)
		{
			if ((SeenComponents & 2) != 0)
			{
				return LevelError("Duplicate level component");
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
					return LevelError("Unknown level body type");
				}

				Entity.BodyType = *Type == "static" ? ELevelBodyType::Static : ELevelBodyType::Dynamic;
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
				return LevelError("Unknown level body type");
			}

			const auto MassKg = ReadNumber<float>((*Body)[1]);
			const auto Friction = ReadNumber<float>((*Body)[2]);
			const auto Restitution = ReadNumber<float>((*Body)[3]);
			const auto LinearDamping = ReadNumber<float>((*Body)[4]);
			const auto AngularDamping = ReadNumber<float>((*Body)[5]);
			const auto GravityScale = ReadNumber<float>((*Body)[6]);
			if (!MassKg || !Friction || !Restitution || !LinearDamping || !AngularDamping || !GravityScale)
			{
				return LevelError("Invalid level rigid body settings");
			}

			Entity.BodyType = *Type == "static" ? ELevelBodyType::Static : ELevelBodyType::Dynamic;
			Entity.BodySettings = {
			    .MassKg = *MassKg,
			    .Friction = *Friction,
			    .Restitution = *Restitution,
			    .LinearDamping = *LinearDamping,
			    .AngularDamping = *AngularDamping,
			    .GravityScale = *GravityScale,
			};
		}
		else if (SchemaVersion >= 4 && Component.key == GetLevelComponentDescriptor(ELevelComponentType::Light).SerializationKey)
		{
			if ((SeenComponents & 4) != 0)
			{
				return LevelError("Duplicate level component");
			}

			SeenComponents |= 4;
			const auto Light = ReadLight(Component.value);
			if (!Light)
			{
				return std::unexpected(Light.error());
			}

			Entity.Light = *Light;
		}
		else if (SchemaVersion >= 4 && Component.key == GetLevelComponentDescriptor(ELevelComponentType::SkyAtmosphere).SerializationKey)
		{
			if ((SeenComponents & 8) != 0)
			{
				return LevelError("Duplicate level component");
			}

			SeenComponents |= 8;
			const auto Atmosphere = ReadAtmosphere(Component.value);
			if (!Atmosphere)
			{
				return std::unexpected(Atmosphere.error());
			}

			Entity.SkyAtmosphere = *Atmosphere;
		}
		else if (SchemaVersion >= 4 && Component.key == GetLevelComponentDescriptor(ELevelComponentType::HeightFog).SerializationKey)
		{
			if ((SeenComponents & 16) != 0)
			{
				return LevelError("Duplicate level component");
			}

			SeenComponents |= 16;
			const auto Fog = ReadFog(Component.value);
			if (!Fog)
			{
				return std::unexpected(Fog.error());
			}

			Entity.HeightFog = *Fog;
		}
		else if (SchemaVersion >= 5 && Component.key == GetLevelComponentDescriptor(ELevelComponentType::SoftBody).SerializationKey)
		{
			if ((SeenComponents & 32) != 0)
			{
				return LevelError("Duplicate level component");
			}

			SeenComponents |= 32;
			const auto SoftBody = ReadSoftBody(Component.value);
			if (!SoftBody)
			{
				return std::unexpected(SoftBody.error());
			}

			Entity.SoftBody = *SoftBody;
		}
		else
		{
			return LevelError("Unknown level component");
		}
	}

	return Entity;
}

std::expected<FLevelFolder, FLevelError> ReadFolder(const simdjson::dom::element Element, std::size_t& MembershipCount)
{
	const auto Fields = ReadFields(Element, std::array<std::string_view, 4>{"id", "name", "parent", "entities"}, 0xf);
	if (!Fields)
	{
		return std::unexpected(Fields.error());
	}

	const auto Id = ReadId<FObjectId>((*Fields)[0]);
	const auto Name = ReadString((*Fields)[1]);
	if (!Id || !Name || Name->empty() || !IsValidName(*Name))
	{
		return LevelError("Invalid level folder ID or name");
	}

	FLevelFolder Folder{.Id = *Id, .Name = std::string(*Name)};
	if (!(*Fields)[2].is_null())
	{
		const auto Parent = ReadId<FObjectId>((*Fields)[2]);
		if (!Parent)
		{
			return std::unexpected(Parent.error());
		}

		Folder.Parent = *Parent;
	}

	simdjson::dom::array Entities;
	if ((*Fields)[3].get_array().get(Entities) || Entities.size() > MaximumLevelEntities - MembershipCount)
	{
		return LevelError("Invalid level folder entities array or membership count");
	}

	MembershipCount += Entities.size();
	Folder.Entities.reserve(Entities.size());

	for (const auto Member : Entities)
	{
		const auto Entity = ReadId<FObjectId>(Member);
		if (!Entity)
		{
			return std::unexpected(Entity.error());
		}

		Folder.Entities.push_back(*Entity);
	}

	return Folder;
}

std::expected<void, FLevelError> ValidateLevelFolders(const FLevelDocument& Document)
{
	if (Document.Folders.empty())
	{
		return {};
	}

	std::map<FObjectId, bool> Entities;

	for (const auto& Entity : Document.Entities)
	{
		Entities.emplace(Entity.Id, false);
	}

	std::map<FObjectId, std::size_t> Indices;
	std::size_t MembershipCount = 0;

	for (std::size_t Index = 0; Index < Document.Folders.size(); ++Index)
	{
		const FLevelFolder& Folder = Document.Folders[Index];
		if (!Folder.Id.IsValid() || Folder.Name.empty() || !IsValidName(Folder.Name) || Entities.contains(Folder.Id))
		{
			return LevelError("Invalid level folder ID or name, or folder ID conflicts with an entity");
		}

		if (!Indices.emplace(Folder.Id, Index).second)
		{
			return LevelError("Duplicate level folder ID");
		}

		if (Folder.Entities.size() > MaximumLevelEntities - MembershipCount)
		{
			return LevelError("Level exceeds the limit of 1000000 folder memberships");
		}

		MembershipCount += Folder.Entities.size();

		for (const FObjectId Member : Folder.Entities)
		{
			const auto Entity = Entities.find(Member);
			if (Entity == Entities.end() || Entity->second)
			{
				return LevelError("Missing level folder entity or duplicate folder membership");
			}

			Entity->second = true;
		}
	}

	std::vector<std::size_t> Parents(Document.Folders.size(), Document.Folders.size());

	for (std::size_t Index = 0; Index < Document.Folders.size(); ++Index)
	{
		const FObjectId Parent = Document.Folders[Index].Parent;
		if (!Parent.IsValid())
		{
			continue;
		}

		const auto Found = Indices.find(Parent);
		if (Found == Indices.end())
		{
			return LevelError("Missing level folder parent");
		}

		Parents[Index] = Found->second;
	}

	std::vector<std::uint8_t> States(Document.Folders.size());

	for (std::size_t Start = 0; Start < Document.Folders.size(); ++Start)
	{
		std::size_t Current = Start;

		while (Current != Document.Folders.size() && States[Current] == 0)
		{
			States[Current] = 1;
			Current = Parents[Current];
		}

		if (Current != Document.Folders.size() && States[Current] == 1)
		{
			return LevelError("Level folders contain a parent cycle");
		}

		Current = Start;

		while (Current != Document.Folders.size() && States[Current] == 1)
		{
			States[Current] = 2;
			Current = Parents[Current];
		}
	}

	return {};
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

using FVisualValue = std::variant<float, bool, FVector3, FAssetId, FObjectId, std::string_view>;

void AppendVisualComponent(std::string& Output, const ELevelComponentType Type, const std::span<const FVisualValue> Values, bool& bHasComponent)
{
	const auto& Descriptor = GetLevelComponentDescriptor(Type);
	AppendFieldKey(Output, bHasComponent ? ",\n        " : "\n        ", Descriptor.SerializationKey);
	Output += '{';
	for (std::size_t Index = 0; Index < Values.size(); ++Index)
	{
		AppendFieldKey(Output, Index == 0 ? "\n          " : ",\n          ", Descriptor.Properties[Index].Key);

		std::visit([&Output](const auto& Value)
		{
			using T = std::decay_t<decltype(Value)>;
			if constexpr (std::is_same_v<T, bool>)
			{
				Output += Value ? "true" : "false";
			}
			else if constexpr (std::is_same_v<T, float>)
			{
				AppendNumber(Output, Value);
			}
			else if constexpr (std::is_same_v<T, FVector3>)
			{
				AppendNumbers(Output, std::array{Value.X, Value.Y, Value.Z});
			}
			else if constexpr (std::is_same_v<T, std::string_view>)
			{
				AppendString(Output, Value);
			}
			else
			{
				if (Value.IsValid())
				{
					AppendString(Output, Value.ToString());
				}
				else
				{
					Output += "null";
				}
			}
		}, Values[Index]);
	}

	Output += "\n        }";
	bHasComponent = true;
}

std::expected<void, FLevelError> WriteTemporaryFile(const std::filesystem::path& Path, const std::string_view Text)
{
#ifdef _WIN32
	const HANDLE File = CreateFileW(Path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (File == INVALID_HANDLE_VALUE)
	{
		return LevelError("Cannot create level temporary file");
	}

	DWORD Written = 0;
	const bool bWritten = WriteFile(File, Text.data(), static_cast<DWORD>(Text.size()), &Written, nullptr) != FALSE && Written == Text.size();
	const bool bFlushed = bWritten && FlushFileBuffers(File) != FALSE;
	const bool bClosed = CloseHandle(File) != FALSE;
#else
	const int File = open(Path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
	if (File < 0)
	{
		return LevelError("Cannot create level temporary file");
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
		return LevelError("Cannot write level temporary file");
	}

	return {};
}
}

std::expected<void, FLevelError> ValidateLevelDocument(const FLevelDocument& Document)
{
	if (!Document.Id.IsValid() || !IsValidName(Document.Name) || Document.Entities.size() > MaximumLevelEntities || Document.Folders.size() > MaximumLevelEntities)
	{
		return LevelError("Invalid level document ID, name, entity count, or folder count");
	}

	const auto Entities = ValidateLevelEntities(Document.Entities);
	if (!Entities)
	{
		return Entities;
	}

	return ValidateLevelFolders(Document);
}

std::expected<std::string, FLevelError> SerializeLevel(const FLevelDocument& Document)
{
	const auto Validation = ValidateLevelDocument(Document);
	if (!Validation)
	{
		return std::unexpected(Validation.error());
	}

	const auto& TransformDescriptor = GetLevelComponentDescriptor(ELevelComponentType::Transform);
	const auto& MeshDescriptor = GetLevelComponentDescriptor(ELevelComponentType::StaticMesh);
	const auto& BodyDescriptor = GetLevelComponentDescriptor(ELevelComponentType::RigidBody);
	std::vector<const FLevelEntity*> Entities;
	Entities.reserve(Document.Entities.size());

	for (const auto& Entity : Document.Entities)
	{
		Entities.push_back(&Entity);
	}

	std::ranges::sort(Entities, [](const FLevelEntity* Left, const FLevelEntity* Right)
	{
		return Left->Id < Right->Id;
	});

	std::string Output = "{\n  \"format\": \"HertaLevel\",\n  \"formatVersion\": 2,\n  \"engineSchemaVersion\": 5,\n  \"id\": ";
	AppendString(Output, Document.Id.ToString());
	Output += ",\n  \"name\": ";
	AppendString(Output, Document.Name);
	Output += ",\n  \"entities\": [";

	for (std::size_t Index = 0; Index < Entities.size(); ++Index)
	{
		const FLevelEntity& Entity = *Entities[Index];
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
			AppendFieldKey(Output, ", ", MeshDescriptor.Properties[1].Key);
			Output += '[';

			for (std::size_t Slot = 0; Slot < Entity.Mesh->Materials.size(); ++Slot)
			{
				Output += Slot == 0 ? "" : ", ";

				if (Entity.Mesh->Materials[Slot].IsValid())
				{
					AppendString(Output, Entity.Mesh->Materials[Slot].ToString());
				}
				else
				{
					Output += "null";
				}
			}

			Output += ']';
			Output += '}';
		}

		if (Entity.BodyType != ELevelBodyType::None)
		{
			Output += Entity.Mesh ? ",\n" : "\n";
			AppendFieldKey(Output, "        ", BodyDescriptor.SerializationKey);
			AppendFieldKey(Output, "{\n          ", BodyDescriptor.Properties[0].Key);
			AppendString(Output, Entity.BodyType == ELevelBodyType::Static ? "static" : "dynamic");
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

		bool bHasComponent = Entity.Mesh.has_value() || Entity.BodyType != ELevelBodyType::None;

		if (Entity.Light)
		{
			const auto& Light = *Entity.Light;
			constexpr std::array<std::string_view, 5> Types{"directional", "sky", "point", "spot", "rect"};
			const std::array<FVisualValue, 17> Values{
			    Types[static_cast<std::size_t>(Light.Type)],
			    Light.Color,
			    Light.Intensity,
			    Light.bUseTemperature,
			    Light.TemperatureKelvin,
			    Light.bCastShadows,
			    Light.ShadowBias,
			    Light.ShadowNormalBias,
			    Light.Range,
			    Light.InnerConeAngle,
			    Light.OuterConeAngle,
			    Light.Width,
			    Light.Height,
			    Light.Environment,
			    Light.AmbientStrength,
			    Light.bEnvironmentVisible,
			    Light.bEnabled,
			};

			AppendVisualComponent(Output, ELevelComponentType::Light, Values, bHasComponent);
		}

		if (Entity.SkyAtmosphere)
		{
			const auto& Atmosphere = *Entity.SkyAtmosphere;
			const std::array<FVisualValue, 7> Values{
			    Atmosphere.Sun,
			    Atmosphere.RayleighScattering,
			    Atmosphere.MieScattering,
			    Atmosphere.MieAnisotropy,
			    Atmosphere.PlanetRadius,
			    Atmosphere.AtmosphereHeight,
			    Atmosphere.bEnabled,
			};

			AppendVisualComponent(Output, ELevelComponentType::SkyAtmosphere, Values, bHasComponent);
		}

		if (Entity.HeightFog)
		{
			const auto& Fog = *Entity.HeightFog;
			constexpr std::array<std::string_view, 3> Qualities{"low", "medium", "high"};
			const std::array<FVisualValue, 8> Values{
			    Fog.Density,
			    Fog.HeightFalloff,
			    Fog.Albedo,
			    Fog.Anisotropy,
			    Fog.MaxDistance,
			    Qualities[static_cast<std::size_t>(Fog.Quality)],
			    Fog.bEnabled,
			    Fog.bVolumetric,
			};

			AppendVisualComponent(Output, ELevelComponentType::HeightFog, Values, bHasComponent);
		}

		if (Entity.SoftBody)
		{
			const auto& SoftBody = *Entity.SoftBody;
			const std::array<FVisualValue, 11> Values{
			    SoftBodyShapes[static_cast<std::size_t>(SoftBody.Shape)],
			    SoftBody.Length,
			    SoftBody.Height,
			    SoftBody.Thickness,
			    SoftBody.MassKg,
			    SoftBody.Stiffness,
			    SoftBody.Pressure,
			    SoftBody.Friction,
			    SoftBody.bPinned,
			    SoftBody.Material,
			    SoftBody.Attachment,
			};

			AppendVisualComponent(Output, ELevelComponentType::SoftBody, Values, bHasComponent);
		}

		Output += bHasComponent ? "\n      }\n    }" : "}\n    }";
		if (Output.size() > MaximumLevelBytes)
		{
			return LevelError("Level exceeds the 64 MiB limit");
		}
	}

	Output += Entities.empty() ? "],\n  \"folders\": [" : "\n  ],\n  \"folders\": [";
	std::vector<const FLevelFolder*> Folders;
	Folders.reserve(Document.Folders.size());

	for (const auto& Folder : Document.Folders)
	{
		Folders.push_back(&Folder);
	}

	std::ranges::sort(Folders, [](const FLevelFolder* Left, const FLevelFolder* Right)
	{
		return Left->Id < Right->Id;
	});

	for (std::size_t Index = 0; Index < Folders.size(); ++Index)
	{
		const FLevelFolder& Folder = *Folders[Index];
		Output += Index == 0 ? "\n" : ",\n";
		Output += "    {\n      \"id\": ";
		AppendString(Output, Folder.Id.ToString());
		Output += ",\n      \"name\": ";
		AppendString(Output, Folder.Name);
		Output += ",\n      \"parent\": ";

		if (Folder.Parent.IsValid())
		{
			AppendString(Output, Folder.Parent.ToString());
		}
		else
		{
			Output += "null";
		}

		Output += ",\n      \"entities\": [";
		std::vector<FObjectId> Members = Folder.Entities;
		std::ranges::sort(Members);

		for (std::size_t MemberIndex = 0; MemberIndex < Members.size(); ++MemberIndex)
		{
			if (MemberIndex != 0)
			{
				Output += ", ";
			}

			AppendString(Output, Members[MemberIndex].ToString());
		}

		Output += "]\n    }";
		if (Output.size() > MaximumLevelBytes)
		{
			return LevelError("Level exceeds the 64 MiB limit");
		}
	}

	Output += Folders.empty() ? "]\n}\n" : "\n  ]\n}\n";
	if (Output.size() > MaximumLevelBytes)
	{
		return LevelError("Level exceeds the 64 MiB limit");
	}

	return Output;
}

std::expected<FLevelDocument, FLevelError> ParseLevel(const std::string_view Text)
{
	if (Text.size() > MaximumLevelBytes)
	{
		return LevelError("Level exceeds the 64 MiB limit");
	}

	simdjson::dom::parser Parser(MaximumLevelBytes);
	simdjson::dom::element Root;
	const auto ParseError = Parser.parse(Text.data(), Text.size()).get(Root);
	if (ParseError == simdjson::MEMALLOC)
	{
		std::terminate();
	}

	if (ParseError)
	{
		return LevelError("Malformed level JSON or invalid UTF-8");
	}

	const auto Fields = ReadFields(Root, std::array<std::string_view, 7>{"format", "formatVersion", "engineSchemaVersion", "id", "name", "entities", "folders"}, 0x3f);
	if (!Fields)
	{
		return std::unexpected(Fields.error());
	}

	const auto Format = ReadString((*Fields)[0]);
	std::uint64_t FormatVersion = 0;
	std::uint64_t SchemaVersion = 0;
	if (!Format || (*Format != "HertaLevel" && *Format != "HertaScene") || (*Fields)[1].get_uint64().get(FormatVersion) || (*Fields)[2].get_uint64().get(SchemaVersion))
	{
		return LevelError("Invalid level format header");
	}

	const std::uint64_t ExpectedFormatVersion = *Format == "HertaScene" ? 1 : 2;
	if (FormatVersion != ExpectedFormatVersion || SchemaVersion < 1 || SchemaVersion > 5 || (*Format == "HertaScene" && SchemaVersion >= 3))
	{
		return LevelError("Unsupported level format or engine schema version; supported formats are HertaLevel 2 with engine schemas 1 through 5 and legacy HertaScene 1 with engine schemas 1 through 2");
	}

	const bool bHasFolders = Root["folders"].error() == simdjson::SUCCESS;
	if ((SchemaVersion >= 3) != bHasFolders)
	{
		return LevelError("Level folders are required in engine schemas 3 and later");
	}

	const auto Id = ReadId<FObjectId>((*Fields)[3]);
	const auto Name = ReadString((*Fields)[4]);
	if (!Id || !Name || !IsValidName(*Name))
	{
		return LevelError("Invalid level document ID or name");
	}

	simdjson::dom::array Entities;
	if ((*Fields)[5].get_array().get(Entities) || Entities.size() > MaximumLevelEntities)
	{
		return LevelError("Invalid level entities array or entity count");
	}

	FLevelDocument Document{.Id = *Id, .Name = std::string(*Name)};
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

	if (SchemaVersion >= 3)
	{
		simdjson::dom::array Folders;
		if ((*Fields)[6].get_array().get(Folders) || Folders.size() > MaximumLevelEntities)
		{
			return LevelError("Invalid level folders array or folder count");
		}

		Document.Folders.reserve(Folders.size());
		std::size_t MembershipCount = 0;

		for (const auto Element : Folders)
		{
			auto Folder = ReadFolder(Element, MembershipCount);
			if (!Folder)
			{
				return std::unexpected(Folder.error());
			}

			Document.Folders.push_back(std::move(*Folder));
		}
	}

	const auto Validation = ValidateLevelDocument(Document);
	if (!Validation)
	{
		return std::unexpected(Validation.error());
	}

	return Document;
}

std::expected<FLevelDocument, FLevelError> LoadLevel(const std::filesystem::path& Path)
{
	std::error_code Error;
	const std::uintmax_t Size = std::filesystem::file_size(Path, Error);
	if (Error || Size > MaximumLevelBytes)
	{
		return LevelError("Cannot read level file or level exceeds the 64 MiB limit");
	}

	std::ifstream Stream(Path, std::ios::binary);
	std::string Text(static_cast<std::size_t>(Size), '\0');
	Stream.read(Text.data(), static_cast<std::streamsize>(Text.size()));
	if (!Stream)
	{
		return LevelError("Cannot read complete level file");
	}

	if (Stream.peek() != std::char_traits<char>::eof() || Stream.bad())
	{
		return LevelError("Level file changed during read or could not be read completely");
	}

	return ParseLevel(Text);
}

std::expected<void, FLevelError> SaveLevel(const std::filesystem::path& Path, const FLevelDocument& Document)
{
	const auto Text = SerializeLevel(Document);
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
		return LevelError("Cannot atomically replace level file");
	}

	return {};
}
}
