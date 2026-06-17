#include <deque>
#include <mutex>
#include <thread>

#include "shader.h"
#include "shader_recompiler.h"
#include "dxc_compiler.h"

#ifdef XENOS_RECOMP_AIR
#include "air_compiler.h"
#endif

static std::unique_ptr<uint8_t[]> readAllBytes(const char* filePath, size_t& fileSize)
{
    FILE* file = fopen(filePath, "rb");
    fseek(file, 0, SEEK_END);
    fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);
    auto data = std::make_unique<uint8_t[]>(fileSize);
    fread(data.get(), 1, fileSize, file);
    fclose(file);
    return data;
}

static void writeAllBytes(const char* filePath, const void* data, size_t dataSize)
{
    FILE* file = fopen(filePath, "wb");
    fwrite(data, 1, dataSize, file);
    fclose(file);
}

static bool tryGetShaderContainerSize(const uint8_t* data, size_t fileSize, size_t offset, size_t& dataSize)
{
    if (offset + sizeof(ShaderContainer) > fileSize)
        return false;

    auto shaderContainer = reinterpret_cast<const ShaderContainer*>(data + offset);
    size_t virtualSize = shaderContainer->virtualSize;
    size_t physicalSize = shaderContainer->physicalSize;
    size_t constantTableOffset = shaderContainer->constantTableOffset;
    size_t shaderOffset = shaderContainer->shaderOffset;

    if ((shaderContainer->flags & 0xFFFFFF00) != 0x102A1100 ||
        virtualSize > fileSize - offset ||
        physicalSize > fileSize - offset - virtualSize ||
        constantTableOffset + sizeof(ConstantTableContainer) > virtualSize ||
        shaderOffset + sizeof(Shader) > virtualSize)
    {
        return false;
    }

    auto shader = reinterpret_cast<const Shader*>(data + offset + shaderOffset);
    size_t physicalOffset = shader->physicalOffset;
    size_t shaderSize = shader->size;
    if (physicalOffset > physicalSize || shaderSize > physicalSize - physicalOffset)
        return false;

    dataSize = virtualSize + physicalSize;
    return true;
}

struct RecompiledShader
{
    uint8_t* data = nullptr;
    IDxcBlob* dxil = nullptr;
    std::vector<uint8_t> spirv;
    std::vector<uint8_t> air;
    uint32_t specConstantsMask = 0;
    bool failed = false;
};

static bool tryRecompile(ShaderRecompiler& recompiler, const uint8_t* data, const std::string_view include)
{
    __try
    {
        recompiler.recompile(data, include);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void recompileShader(RecompiledShader& shader, XXH64_hash_t hash, const std::string& filename, const std::string_view include, std::atomic<uint32_t>& progress, uint32_t numShaders)
{
    try
    {
        thread_local ShaderRecompiler recompiler;
        recompiler = {};
        if (!tryRecompile(recompiler, shader.data, include))
        {
            shader.failed = true;
            fmt::println(stderr, "Skipping shader {:016X} from {}: structured exception in recompiler", hash, filename);
            ++progress;
            return;
        }

        shader.specConstantsMask = recompiler.specConstantsMask;

        thread_local DxcCompiler dxcCompiler;

#ifdef XENOS_RECOMP_DXIL
        shader.dxil = dxcCompiler.compile(recompiler.out, recompiler.isPixelShader, recompiler.specConstantsMask != 0, false);
        if (shader.dxil == nullptr)
        {
            shader.failed = true;
            fmt::println(stderr, "Skipping shader {:016X} from {}: DXIL compilation failed", hash, filename);
            ++progress;
            return;
        }
        if (*(reinterpret_cast<uint32_t *>(shader.dxil->GetBufferPointer()) + 1) == 0)
        {
            shader.failed = true;
            fmt::println(stderr, "Skipping shader {:016X} from {}: DXIL was not signed", hash, filename);
            shader.dxil->Release();
            shader.dxil = nullptr;
            ++progress;
            return;
        }
#endif

#ifdef XENOS_RECOMP_AIR
        shader.air = AirCompiler::compile(recompiler.out);
#endif

        IDxcBlob* spirv = dxcCompiler.compile(recompiler.out, recompiler.isPixelShader, false, true);
        if (spirv == nullptr)
        {
            shader.failed = true;
            fmt::println(stderr, "Skipping shader {:016X} from {}: SPIR-V compilation failed", hash, filename);
            ++progress;
            return;
        }

        bool result = smolv::Encode(spirv->GetBufferPointer(), spirv->GetBufferSize(), shader.spirv, smolv::kEncodeFlagStripDebugInfo);
        if (!result)
        {
            spirv->Release();
            shader.failed = true;
            fmt::println(stderr, "Skipping shader {:016X} from {}: SPIR-V compression failed", hash, filename);
            ++progress;
            return;
        }

        spirv->Release();
    }
    catch (const std::exception& e)
    {
        shader.failed = true;
        fmt::println(stderr, "Skipping shader {:016X} from {}: {}", hash, filename, e.what());
        ++progress;
        return;
    }
    catch (...)
    {
        shader.failed = true;
        fmt::println(stderr, "Skipping shader {:016X} from {}: unknown recompilation failure", hash, filename);
        ++progress;
        return;
    }

    size_t currentProgress = ++progress;
    if ((currentProgress % 10) == 0 || (currentProgress == numShaders - 1))
        fmt::println("Recompiling shaders... {}%", currentProgress / float(numShaders) * 100.0f);
}

static uint32_t dumpHlslShaders(const char* input, const char* output, const std::string_view include)
{
    std::filesystem::create_directories(output);

    std::vector<std::filesystem::path> inputFiles;
    if (std::filesystem::is_directory(input))
    {
        for (auto& file : std::filesystem::recursive_directory_iterator(input))
        {
            if (!std::filesystem::is_directory(file))
                inputFiles.emplace_back(file.path());
        }
    }
    else
    {
        inputFiles.emplace_back(input);
    }

    std::map<XXH64_hash_t, bool> dumpedShaders;
    uint32_t dumpedCount = 0;

    for (auto& inputFile : inputFiles)
    {
        size_t fileSize = 0;
        auto fileData = readAllBytes(inputFile.string().c_str(), fileSize);

        for (size_t i = 0; fileSize > sizeof(ShaderContainer) && i < fileSize - sizeof(ShaderContainer) - 1;)
        {
            size_t dataSize = 0;
            if (tryGetShaderContainerSize(fileData.get(), fileSize, i, dataSize))
            {
                auto shaderContainer = reinterpret_cast<const ShaderContainer*>(fileData.get() + i);
                XXH64_hash_t hash = XXH3_64bits(shaderContainer, dataSize);
                if (dumpedShaders.find(hash) == dumpedShaders.end())
                {
                    ShaderRecompiler recompiler;
                    recompiler.recompile(fileData.get() + i, include);

                    std::filesystem::path outputPath = output;
                    outputPath /= fmt::format("{:016X}.hlsl", hash);
                    writeAllBytes(outputPath.string().c_str(), recompiler.out.data(), recompiler.out.size());

                    dumpedShaders.emplace(hash, true);
                    ++dumpedCount;
                }

                i += dataSize;
            }
            else
            {
                // Byte-by-byte: shader containers are not guaranteed 4-byte
                // aligned in the data, so stepping by a DWORD misses them.
                ++i;
            }
        }
    }

    return dumpedCount;
}

int main(int argc, char** argv)
{
#ifndef XENOS_RECOMP_INPUT
    if (argc < 4)
    {
        printf("Usage: XenosRecomp [input path] [output path] [shader common header file path]\n");
        printf("       XenosRecomp --dump-hlsl [input path] [output directory] [shader common header file path]");
        return 0;
    }
#endif

    if (strcmp(argv[1], "--dump-hlsl") == 0)
    {
        if (argc < 5)
        {
            printf("Usage: XenosRecomp --dump-hlsl [input path] [output directory] [shader common header file path]");
            return 0;
        }

        size_t includeSize = 0;
        auto includeData = readAllBytes(argv[4], includeSize);
        std::string_view include(reinterpret_cast<const char*>(includeData.get()), includeSize);

        uint32_t dumpedCount = dumpHlslShaders(argv[2], argv[3], include);
        fmt::println("Dumped {} HLSL shaders", dumpedCount);
        return 0;
    }

    const char* input =
#ifdef XENOS_RECOMP_INPUT 
        XENOS_RECOMP_INPUT
#else
        argv[1]
#endif
    ;

    const char* output =
#ifdef XENOS_RECOMP_OUTPUT 
        XENOS_RECOMP_OUTPUT
#else
        argv[2]
#endif
        ;
    
    const char* includeInput =
#ifdef XENOS_RECOMP_INCLUDE_INPUT
        XENOS_RECOMP_INCLUDE_INPUT
#else
        argv[3]
#endif
        ;

    uint32_t threadCount = std::max(std::thread::hardware_concurrency(), 1u);
#ifndef XENOS_RECOMP_INPUT
    for (int i = 4; i + 1 < argc; i++)
    {
        if (strcmp(argv[i], "-j") == 0 || strcmp(argv[i], "--jobs") == 0)
        {
            threadCount = std::max<uint32_t>(uint32_t(std::strtoul(argv[i + 1], nullptr, 10)), 1u);
            ++i;
        }
    }
#endif

    size_t includeSize = 0;
    auto includeData = readAllBytes(includeInput, includeSize);
    std::string_view include(reinterpret_cast<const char*>(includeData.get()), includeSize);

    if (std::filesystem::is_directory(input))
    {
        std::vector<std::unique_ptr<uint8_t[]>> files;
        std::map<XXH64_hash_t, RecompiledShader> shaders;
        std::map<XXH64_hash_t, std::string> shaderFilenames;

        for (auto& file : std::filesystem::recursive_directory_iterator(input))
        {
            if (std::filesystem::is_directory(file))
            {
                continue;
            }
            
            size_t fileSize = 0;
            auto fileData = readAllBytes(file.path().string().c_str(), fileSize);
            bool foundAny = false;

            for (size_t i = 0; fileSize > sizeof(ShaderContainer) && i < fileSize - sizeof(ShaderContainer) - 1;)
            {
                size_t dataSize = 0;
                if (tryGetShaderContainerSize(fileData.get(), fileSize, i, dataSize))
                {
                    auto shaderContainer = reinterpret_cast<const ShaderContainer*>(fileData.get() + i);
                    XXH64_hash_t hash = XXH3_64bits(shaderContainer, dataSize);
                    auto shader = shaders.try_emplace(hash);
                    if (shader.second)
                    {
                        shader.first->second.data = fileData.get() + i;
                        foundAny = true;
                        shaderFilenames[hash] = file.path().string();
                    }

                    i += dataSize;
                }
                else
                {
                    // Byte-by-byte: shader containers are not guaranteed 4-byte
                    // aligned in the data, so stepping by a DWORD misses them.
                    ++i;
                }
            }

            if (foundAny)
                files.emplace_back(std::move(fileData));
        }

        std::mutex shaderQueueMutex;
        std::deque<XXH64_hash_t> shaderQueue;
        for (const auto& [hash, _] : shaders)
        {
            shaderQueue.emplace_back(hash);
        }

        const uint32_t numThreads = threadCount;
        fmt::println("Recompiling shaders with {} threads", numThreads);

        std::atomic<uint32_t> progress = 0;
        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        for (uint32_t i = 0; i < numThreads; i++)
        {
            threads.emplace_back([&]
            {
                while (true)
                {
                    XXH64_hash_t shaderHash;
                    {
                        std::lock_guard lock(shaderQueueMutex);
                        if (shaderQueue.empty()) {
                            return;
                        }
                        shaderHash = shaderQueue.front();
                        shaderQueue.pop_front();
                    }
                    recompileShader(shaders[shaderHash], shaderHash, shaderFilenames[shaderHash], include, progress, shaders.size());
                }
            });
        }
        for (auto& thread : threads)
        {
            thread.join();
        }

        fmt::println("Creating shader cache...");

        StringBuffer f;
        f.println("#include \"shader_cache.h\"");
        f.println("ShaderCacheEntry g_shaderCacheEntries[] = {{");

        std::vector<uint8_t> dxil;
        std::vector<uint8_t> spirv;
        std::vector<uint8_t> air;
        size_t writtenShaderCount = 0;

        for (auto& [hash, shader] : shaders)
        {
            if (shader.failed)
            {
                continue;
            }

            const std::string& fullFilename = shaderFilenames[hash];
            std::string filename = fullFilename;
            size_t shaderPos = filename.find("shader");
            if (shaderPos != std::string::npos) {
                filename = filename.substr(shaderPos);
            }

            // Prevent bad escape sequences in Windows shader path metadata.
            std::replace(filename.begin(), filename.end(), '\\', '/');

            f.println("\t{{ 0x{:X}, {}, {}, {}, {}, {}, {}, {}, \"{}\" }},",
                hash, dxil.size(), (shader.dxil != nullptr) ? shader.dxil->GetBufferSize() : 0,
                spirv.size(), shader.spirv.size(), air.size(), shader.air.size(), shader.specConstantsMask, filename);

            if (shader.dxil != nullptr)
            {
                dxil.insert(dxil.end(), reinterpret_cast<uint8_t *>(shader.dxil->GetBufferPointer()),
                    reinterpret_cast<uint8_t *>(shader.dxil->GetBufferPointer()) + shader.dxil->GetBufferSize());
            }

#ifdef XENOS_RECOMP_AIR
            air.insert(air.end(), shader.air.begin(), shader.air.end());
#endif

            spirv.insert(spirv.end(), shader.spirv.begin(), shader.spirv.end());
            ++writtenShaderCount;
        }

        f.println("}};");

        fmt::println("Compressing DXIL cache...");

        int level = ZSTD_maxCLevel();

#ifdef XENOS_RECOMP_DXIL
        std::vector<uint8_t> dxilCompressed(ZSTD_compressBound(dxil.size()));
        dxilCompressed.resize(ZSTD_compress(dxilCompressed.data(), dxilCompressed.size(), dxil.data(), dxil.size(), level));

        f.print("const uint8_t g_compressedDxilCache[] = {{");

        for (auto data : dxilCompressed)
            f.print("{},", data);

        f.println("}};");
        f.println("const size_t g_dxilCacheCompressedSize = {};", dxilCompressed.size());
        f.println("const size_t g_dxilCacheDecompressedSize = {};", dxil.size());
#endif

#ifdef XENOS_RECOMP_AIR
        fmt::println("Compressing AIR cache...");

        std::vector<uint8_t> airCompressed(ZSTD_compressBound(air.size()));
        airCompressed.resize(ZSTD_compress(airCompressed.data(), airCompressed.size(), air.data(), air.size(), level));

        f.print("const uint8_t g_compressedAirCache[] = {{");

        for (auto data : airCompressed)
            f.print("{},", data);

        f.println("}};");
        f.println("const size_t g_airCacheCompressedSize = {};", airCompressed.size());
        f.println("const size_t g_airCacheDecompressedSize = {};", air.size());
#endif

        fmt::println("Compressing SPIRV cache...");

        std::vector<uint8_t> spirvCompressed(ZSTD_compressBound(spirv.size()));
        spirvCompressed.resize(ZSTD_compress(spirvCompressed.data(), spirvCompressed.size(), spirv.data(), spirv.size(), level));

        f.print("const uint8_t g_compressedSpirvCache[] = {{");

        for (auto data : spirvCompressed)
            f.print("{},", data);

        f.println("}};");

        f.println("const size_t g_spirvCacheCompressedSize = {};", spirvCompressed.size());
        f.println("const size_t g_spirvCacheDecompressedSize = {};", spirv.size());
        f.println("const size_t g_shaderCacheEntryCount = {};", writtenShaderCount);

        writeAllBytes(output, f.out.data(), f.out.size());
    }
    else
    {
        try
        {
            ShaderRecompiler recompiler;
            size_t fileSize;
            recompiler.recompile(readAllBytes(input, fileSize).get(), include);
            writeAllBytes(output, recompiler.out.data(), recompiler.out.size());
        }
        catch (const std::exception& e)
        {
            fmt::println(stderr, "Failed to recompile {}: {}", input, e.what());
            return 1;
        }
    }

    return 0;
}
