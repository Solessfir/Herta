#include "Herta/ShaderCompiler/ShaderCompiler.h"

#include "Herta/RHI/Graphics.h"

#include <slang-com-ptr.h>
#include <slang.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <system_error>

#ifdef _WIN32
	#include <Windows.h>
#else
	#include <unistd.h>
#endif

namespace Herta
{
namespace
{
std::string Utf8Path(const std::filesystem::path& Path)
{
	const auto Text = Path.generic_u8string();
	return {Text.begin(), Text.end()};
}

FShaderError CompilerError(const char* const Operation, const Slang::ComPtr<slang::IBlob>& Diagnostics)
{
	std::string Message = Operation;
	if (Diagnostics && Diagnostics->getBufferSize() > 0)
	{
		Message += ": ";
		Message.append(static_cast<const char*>(Diagnostics->getBufferPointer()), Diagnostics->getBufferSize());
	}

	return {std::move(Message)};
}

std::expected<std::vector<std::byte>, FShaderError> ReadSource(const std::filesystem::path& Path)
{
	std::ifstream Input(Path, std::ios::binary | std::ios::ate);
	if (!Input)
	{
		return std::unexpected(FShaderError{"Cannot open shader dependency: " + Utf8Path(Path)});
	}

	const auto Size = Input.tellg();
	if (Size < 0 || Size > 16 * 1024 * 1024)
	{
		return std::unexpected(FShaderError{"Shader dependency exceeds the 16 MiB limit: " + Utf8Path(Path)});
	}

	std::vector<std::byte> Bytes(static_cast<std::size_t>(Size));
	Input.seekg(0);
	if (!Input.read(reinterpret_cast<char*>(Bytes.data()), Size))
	{
		return std::unexpected(FShaderError{"Cannot read shader dependency: " + Utf8Path(Path)});
	}

	return Bytes;
}
}

std::expected<FShaderAsset, FShaderError> CompileShader(const FShaderCompileRequest& Request)
{
	if (Request.EntryPoint.empty() || Request.Stage > EShaderStage::Fragment)
	{
		return std::unexpected(FShaderError{"Shader compilation requires a valid stage and entry point"});
	}

	std::error_code Error;
	const std::filesystem::path Source = std::filesystem::absolute(Request.Source, Error).lexically_normal();
	if (Error)
	{
		return std::unexpected(FShaderError{"Cannot resolve shader source: " + Error.message()});
	}

	const auto SourceBytes = ReadSource(Source);
	if (!SourceBytes)
	{
		return std::unexpected(SourceBytes.error());
	}

	Slang::ComPtr<slang::IGlobalSession> GlobalSession;
	if (SLANG_FAILED(slang::createGlobalSession(GlobalSession.writeRef())))
	{
		return std::unexpected(FShaderError{"Cannot create Slang compiler session"});
	}

	slang::CompilerOptionEntry Options[3]{};
	Options[0].name = slang::CompilerOptionName::VulkanUseEntryPointName;
	Options[0].value.intValue0 = 1;
	Options[1].name = slang::CompilerOptionName::DebugInformation;
	Options[1].value.intValue0 = Request.bDebugInformation ? SLANG_DEBUG_INFO_LEVEL_MINIMAL : SLANG_DEBUG_INFO_LEVEL_NONE;
	Options[2].name = slang::CompilerOptionName::Optimization;
	Options[2].value.intValue0 = SLANG_OPTIMIZATION_LEVEL_HIGH;
	slang::TargetDesc Target;
	Target.format = SLANG_SPIRV;
	Target.profile = GlobalSession->findProfile("spirv_1_5");
	Target.compilerOptionEntries = Options;
	Target.compilerOptionEntryCount = 3;
	const std::string Directory = Utf8Path(Source.parent_path());
	std::vector<std::string> Directories{Directory};
	if (Request.IncludeRoots.size() > 16)
	{
		return std::unexpected(FShaderError{"Shader compilation supports at most 16 include roots"});
	}

	for (const auto& Root : Request.IncludeRoots)
	{
		const auto Absolute = std::filesystem::absolute(Root, Error).lexically_normal();
		if (Error || !std::filesystem::is_directory(Absolute, Error))
		{
			return std::unexpected(FShaderError{"Shader include root is unavailable"});
		}

		const auto Text = Absolute.generic_u8string();
		Directories.emplace_back(Text.begin(), Text.end());
	}

	std::vector<const char*> SearchPaths;
	SearchPaths.reserve(Directories.size());

	for (const auto& Path : Directories)
	{
		SearchPaths.push_back(Path.c_str());
	}

	slang::SessionDesc SessionDescription;
	SessionDescription.targets = &Target;
	SessionDescription.targetCount = 1;
	SessionDescription.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
	SessionDescription.searchPaths = SearchPaths.data();
	SessionDescription.searchPathCount = static_cast<SlangInt>(SearchPaths.size());
	Slang::ComPtr<slang::ISession> Session;
	if (SLANG_FAILED(GlobalSession->createSession(SessionDescription, Session.writeRef())))
	{
		return std::unexpected(FShaderError{"Cannot create Slang compilation session"});
	}

	Slang::ComPtr<slang::IBlob> Diagnostics;
	// A relative source identity keeps cooked metadata independent of the checkout location.
	const std::string SourceName = Utf8Path(Source.filename());
	std::string SourceText(reinterpret_cast<const char*>(SourceBytes->data()), SourceBytes->size());
	slang::IModule* const Module = Session->loadModuleFromSourceString(Utf8Path(Source.stem()).c_str(), SourceName.c_str(), SourceText.c_str(), Diagnostics.writeRef());
	if (Module == nullptr)
	{
		return std::unexpected(CompilerError("Slang source compilation failed", Diagnostics));
	}

	Slang::ComPtr<slang::IEntryPoint> Entry;
	if (SLANG_FAILED(Module->findEntryPointByName(Request.EntryPoint.c_str(), Entry.writeRef())))
	{
		return std::unexpected(FShaderError{"Shader entry point was not found: " + Request.EntryPoint});
	}

	slang::IComponentType* const Components[] = {Module, Entry};
	Slang::ComPtr<slang::IComponentType> Program;
	if (SLANG_FAILED(Session->createCompositeComponentType(Components, 2, Program.writeRef(), Diagnostics.writeRef())))
	{
		return std::unexpected(CompilerError("Slang program composition failed", Diagnostics));
	}

	Slang::ComPtr<slang::IComponentType> Linked;
	if (SLANG_FAILED(Program->link(Linked.writeRef(), Diagnostics.writeRef())))
	{
		return std::unexpected(CompilerError("Slang linking failed", Diagnostics));
	}

	slang::ProgramLayout* const Layout = Linked->getLayout(0, Diagnostics.writeRef());
	const SlangStage ExpectedStage = Request.Stage == EShaderStage::Vertex ? SLANG_STAGE_VERTEX : SLANG_STAGE_FRAGMENT;
	if (Layout == nullptr || Layout->getEntryPointCount() != 1 || Layout->getEntryPointByIndex(0)->getStage() != ExpectedStage)
	{
		return std::unexpected(FShaderError{"Shader entry point stage does not match the requested stage"});
	}

	FShaderAsset Shader;
	Shader.Stage = Request.Stage;
	Shader.EntryPoint = Request.EntryPoint;
	Shader.CompilerVersion = spGetBuildTagString();
	Shader.PermutationKey = "spirv_1_5;column-major;optimization=high;debug=" + std::to_string(Request.bDebugInformation);
	Shader.bDebugInformation = Request.bDebugInformation;
	for (unsigned Index = 0; Index < Layout->getParameterCount(); ++Index)
	{
		auto* const Parameter = Layout->getParameterByIndex(Index);
		auto* const Type = Parameter->getTypeLayout();
		if (Parameter->getCategory() == slang::ParameterCategory::PushConstantBuffer)
		{
			const std::size_t Size = Type->getElementTypeLayout()->getSize();
			if (Size > 128 || Shader.PushConstantSize != 0)
			{
				return std::unexpected(FShaderError{"Only one push constant block of at most 128 bytes is supported"});
			}

			Shader.PushConstantSize = static_cast<std::uint32_t>(Size);
			continue;
		}

		EShaderBindingType BindingType;
		switch (Type->getKind())
		{
			case slang::TypeReflection::Kind::Resource:
				if (Type->getType()->getResourceShape() != SLANG_TEXTURE_2D || Type->getType()->getResourceAccess() != SLANG_RESOURCE_ACCESS_READ)
				{
					return std::unexpected(FShaderError{"Only read-only Texture2D shader resources are currently supported: " + std::string(Parameter->getName())});
				}

				BindingType = EShaderBindingType::Texture;
				break;
			case slang::TypeReflection::Kind::SamplerState:
				BindingType = EShaderBindingType::Sampler;
				break;
			case slang::TypeReflection::Kind::ConstantBuffer:
				if (Type->getElementTypeLayout()->getSize() == 0 || Type->getElementTypeLayout()->getSize() > MaximumGraphicsUniformBytes)
				{
					return std::unexpected(FShaderError{"Shader uniforms must fit the 16 KiB graphics binding limit"});
				}

				BindingType = EShaderBindingType::ConstantBuffer;
				break;
			default:
				return std::unexpected(FShaderError{"Unsupported reflected shader parameter: " + std::string(Parameter->getName())});
		}

		const std::uint32_t Binding = Parameter->getBindingIndex();
		if (Parameter->getBindingSpace() != 0 || (BindingType == EShaderBindingType::Texture ? Binding >= MaximumGraphicsTextures : BindingType == EShaderBindingType::Sampler ? Binding != 128
		                                                                                                                                                                       : Binding != 64)
		    || std::ranges::any_of(Shader.Bindings, [Binding](const FShaderBinding& Existing)
		{
			return Existing.Binding == Binding;
		}))
		{
			return std::unexpected(FShaderError{"Shader resources must use unique bindings t0..t15, b0 at 64, and s0 at 128 in space 0"});
		}

		Shader.Bindings.push_back({.Name = Parameter->getName(), .Type = BindingType, .Binding = Binding, .Space = Parameter->getBindingSpace(), .ByteSize = BindingType == EShaderBindingType::ConstantBuffer ? static_cast<std::uint32_t>(Type->getElementTypeLayout()->getSize()) : 0u});
	}

	std::map<std::string, std::uint64_t> Dependencies;
	Dependencies.emplace(SourceName, HashShaderContent(*SourceBytes));
	for (SlangInt ModuleIndex = 0; ModuleIndex < Session->getLoadedModuleCount(); ++ModuleIndex)
	{
		slang::IModule* const DependencyModule = Session->getLoadedModule(ModuleIndex);
		for (SlangInt32 Index = 0; Index < DependencyModule->getDependencyFileCount(); ++Index)
		{
			const std::string_view DependencyText(DependencyModule->getDependencyFilePath(Index));
			std::filesystem::path DependencyPath(std::u8string(DependencyText.begin(), DependencyText.end()));
			if (DependencyPath.is_relative())
			{
				const auto Relative = DependencyPath;
				DependencyPath = Source.parent_path() / Relative;
				if (!std::filesystem::is_regular_file(DependencyPath, Error))
				{
					for (const auto& Root : Request.IncludeRoots)
					{
						const auto Candidate = Root / Relative;
						if (std::filesystem::is_regular_file(Candidate, Error))
						{
							DependencyPath = Candidate;
							break;
						}
					}
				}
			}

			const auto Data = ReadSource(DependencyPath);
			if (!Data)
			{
				return std::unexpected(Data.error());
			}

			Dependencies[Utf8Path(std::filesystem::absolute(DependencyPath, Error).lexically_normal().lexically_relative(Source.parent_path()))] = HashShaderContent(*Data);
		}
	}

	for (const auto& [Path, Hash] : Dependencies)
	{
		Shader.Dependencies.push_back({.Path = Path, .ContentHash = Hash});
	}

	Slang::ComPtr<slang::IBlob> Bytecode;
	if (SLANG_FAILED(Linked->getEntryPointCode(0, 0, Bytecode.writeRef(), Diagnostics.writeRef())) || !Bytecode || Bytecode->getBufferSize() % 4 != 0)
	{
		return std::unexpected(CompilerError("Slang SPIR-V generation failed", Diagnostics));
	}

	Shader.Bytecode.resize(Bytecode->getBufferSize() / 4);
	std::memcpy(Shader.Bytecode.data(), Bytecode->getBufferPointer(), Bytecode->getBufferSize());
	return Shader;
}

std::expected<void, FShaderError> SaveCookedShader(const std::filesystem::path& Path, const FShaderAsset& Shader)
{
	const auto Bytes = SerializeCookedShader(Shader);
	if (!Bytes)
	{
		return std::unexpected(Bytes.error());
	}

	std::error_code Error;
	if (!Path.parent_path().empty())
	{
		std::filesystem::create_directories(Path.parent_path(), Error);
	}

	if (Error)
	{
		return std::unexpected(FShaderError{"Cannot create shader output directory: " + Error.message()});
	}

#ifdef _WIN32
	const auto ProcessId = GetCurrentProcessId();
#else
	const auto ProcessId = getpid();
#endif
	std::filesystem::path Temporary = Path;
	Temporary += "." + std::to_string(ProcessId) + ".tmp";
	{
		std::ofstream Output(Temporary, std::ios::binary | std::ios::trunc);
		Output.write(reinterpret_cast<const char*>(Bytes->data()), static_cast<std::streamsize>(Bytes->size()));
		Output.close();
		if (!Output)
		{
			std::filesystem::remove(Temporary, Error);
			return std::unexpected(FShaderError{"Cannot write cooked shader: " + Utf8Path(Path)});
		}
	}

#ifdef _WIN32
	if (!MoveFileExW(Temporary.c_str(), Path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		Error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
	}
#else
	std::filesystem::rename(Temporary, Path, Error);
#endif
	if (Error)
	{
		const std::string Message = Error.message();
		std::filesystem::remove(Temporary, Error);
		return std::unexpected(FShaderError{"Cannot publish cooked shader: " + Message});
	}

	return {};
}
}
