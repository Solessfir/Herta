-- Protobuf 21.12, the last release without an Abseil dependency, for GameNetworkingSockets' wire messages. The source lists come from
-- Protobuf's own CMake files, so they stay exact for the pinned revision. protoc builds from the same source, so generated code always matches
-- the runtime it links against and Setup downloads nothing for it.
local function ReadProtobufSources(ProtobufRoot, CMakeFile, Variable)
    local File = io.open(path.join(ProtobufRoot, "cmake", CMakeFile), "rb")
    if not File then
        error("Protobuf is missing " .. CMakeFile .. "; run Setup to fetch submodules")
    end

    -- A Windows checkout may have converted the CMake files to CRLF.
    local Text = (File:read("*a"):gsub("\r", ""))
    File:close()
    local Block = Text:match("set%(" .. Variable .. "\n(.-)%)")
    if not Block then
        error("Cannot find " .. Variable .. " in Protobuf's " .. CMakeFile)
    end

    local Sources = {}
    for Line in Block:gmatch("[^\n]+") do
        local Source = Line:match("%${protobuf_SOURCE_DIR}/(%S+%.cc)")
        if Source then
            table.insert(Sources, path.join(ProtobufRoot, Source))
        end
    end

    return Sources
end

function HertaProtobuf()
    local RepositoryRoot = path.getabsolute(_MAIN_SCRIPT_DIR)
    local ProtobufRoot = path.join(RepositoryRoot, "External/protobuf")
    local ProjectFilesRoot = path.join(RepositoryRoot, "Intermediate/ProjectFiles", (_ACTION or "NoAction") .. HertaToolsetSuffix)

    local function ProtobufProject(Name, Kind, Sources)
        project(Name)
            kind(Kind)
            location(path.join(ProjectFilesRoot, Name))
            targetdir(path.join(RepositoryRoot, "Binaries/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}" .. HertaToolsetSuffix))
            objdir(path.join(RepositoryRoot, "Intermediate/Build/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}" .. HertaToolsetSuffix .. "/%{prj.name}"))
            language "C++"
            -- Protobuf 21 predates C++20's removals.
            cppdialect "C++17"
            warnings "Off"
            files(Sources)
            externalincludedirs { path.join(ProtobufRoot, "src") }

            filter "system:linux"
                defines { "HAVE_PTHREAD" }

            filter "system:windows"
                buildoptions { "/bigobj" }

            filter {}
    end

    local Runtime = ReadProtobufSources(ProtobufRoot, "libprotobuf-lite.cmake", "libprotobuf_lite_files")
    for _, Source in ipairs(ReadProtobufSources(ProtobufRoot, "libprotobuf.cmake", "libprotobuf_files")) do
        table.insert(Runtime, Source)
    end

    ProtobufProject("Protobuf", "StaticLib", Runtime)

    local Compiler = ReadProtobufSources(ProtobufRoot, "libprotoc.cmake", "libprotoc_files")
    table.insert(Compiler, path.join(ProtobufRoot, "src/google/protobuf/compiler/main.cc"))
    ProtobufProject("Protoc", "ConsoleApp", Compiler)
        links { "Protobuf" }
        dependson { "Protobuf" }

        filter "system:linux"
            links { "pthread" }

        filter {}
end
