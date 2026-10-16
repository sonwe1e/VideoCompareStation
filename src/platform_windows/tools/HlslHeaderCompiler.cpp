#include <array>
#include <cstdint>
#include <cstdlib>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <wrl/client.h>

namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kCompilerFlags =
    D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS | D3DCOMPILE_OPTIMIZATION_LEVEL3;

struct Arguments {
    std::filesystem::path composePath;
    std::filesystem::path nv12Path;
    std::filesystem::path probePath;
    std::filesystem::path outputPath;
};

struct ShaderRequest {
    const std::filesystem::path* path;
    const char* entryPoint;
    const char* profile;
};

struct ExpectedSignatureParameter {
    std::string_view semanticName;
    UINT semanticIndex;
    UINT registerIndex;
    BYTE mask;
};

[[nodiscard]] std::string blobText(ID3DBlob* blob) {
    if (blob == nullptr || blob->GetBufferSize() == 0) {
        return {};
    }

    const auto* begin = static_cast<const char*>(blob->GetBufferPointer());
    std::string text{begin, begin + blob->GetBufferSize()};
    while (!text.empty() && text.back() == '\0') {
        text.pop_back();
    }
    return text;
}

[[nodiscard]] std::string formatHresult(HRESULT result) {
    std::ostringstream text;
    text << "0x" << std::hex << std::uppercase << static_cast<std::uint32_t>(result);
    return text.str();
}

[[nodiscard]] std::vector<std::uint8_t> compileShader(const ShaderRequest& request) {
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> diagnostics;
    const HRESULT result = D3DCompileFromFile(request.path->c_str(),
                                              nullptr,
                                              D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                              request.entryPoint,
                                              request.profile,
                                              kCompilerFlags,
                                              0,
                                              &bytecode,
                                              &diagnostics);

    if (FAILED(result)) {
        std::ostringstream message;
        message << "HLSL compilation failed for '" << request.path->string() << "' entry '"
                << request.entryPoint << "' profile '" << request.profile << "' ("
                << formatHresult(result) << ")";
        const std::string diagnosticsText = blobText(diagnostics.Get());
        if (!diagnosticsText.empty()) {
            message << ":\n" << diagnosticsText;
        }
        throw std::runtime_error{message.str()};
    }
    if (bytecode == nullptr || bytecode->GetBufferSize() == 0) {
        throw std::runtime_error{"HLSL compiler returned empty bytecode for entry '" +
                                 std::string{request.entryPoint} + "'."};
    }

    const auto* begin = static_cast<const std::uint8_t*>(bytecode->GetBufferPointer());
    return {begin, begin + bytecode->GetBufferSize()};
}

void validateSignature(std::span<const std::uint8_t> bytecode,
                       std::span<const ExpectedSignatureParameter> expected,
                       const bool outputSignature,
                       std::string_view shaderName) {
    void* reflected = nullptr;
    const HRESULT reflectionResult =
        D3DReflect(bytecode.data(), bytecode.size(), IID_ID3D11ShaderReflection, &reflected);
    if (FAILED(reflectionResult) || reflected == nullptr) {
        throw std::runtime_error{"Could not reflect the " + std::string{shaderName} +
                                 " signature (" + formatHresult(reflectionResult) + ")."};
    }
    ComPtr<ID3D11ShaderReflection> reflection;
    reflection.Attach(static_cast<ID3D11ShaderReflection*>(reflected));

    D3D11_SHADER_DESC shaderDescription{};
    const HRESULT descriptionResult = reflection->GetDesc(&shaderDescription);
    if (FAILED(descriptionResult)) {
        throw std::runtime_error{"Could not inspect the " + std::string{shaderName} +
                                 " signature (" + formatHresult(descriptionResult) + ")."};
    }

    const UINT parameterCount =
        outputSignature ? shaderDescription.OutputParameters : shaderDescription.InputParameters;
    if (static_cast<std::size_t>(parameterCount) != expected.size()) {
        throw std::runtime_error{std::string{shaderName} + " exposes " +
                                 std::to_string(parameterCount) + " signature parameters; " +
                                 std::to_string(expected.size()) + " are required."};
    }

    for (UINT index = 0U; index < parameterCount; ++index) {
        D3D11_SIGNATURE_PARAMETER_DESC parameter{};
        const HRESULT parameterResult = outputSignature
                                            ? reflection->GetOutputParameterDesc(index, &parameter)
                                            : reflection->GetInputParameterDesc(index, &parameter);
        if (FAILED(parameterResult) || parameter.SemanticName == nullptr) {
            throw std::runtime_error{"Could not inspect parameter " + std::to_string(index) +
                                     " of the " + std::string{shaderName} + " signature."};
        }

        const ExpectedSignatureParameter& required = expected[index];
        if (required.semanticName != parameter.SemanticName ||
            required.semanticIndex != parameter.SemanticIndex ||
            required.registerIndex != parameter.Register || required.mask != parameter.Mask) {
            std::ostringstream message;
            message << shaderName << " signature parameter " << index << " is "
                    << parameter.SemanticName << parameter.SemanticIndex << " at register "
                    << parameter.Register << " mask " << static_cast<unsigned int>(parameter.Mask)
                    << "; expected " << required.semanticName << required.semanticIndex
                    << " at register " << required.registerIndex << " mask "
                    << static_cast<unsigned int>(required.mask) << '.';
            throw std::runtime_error{message.str()};
        }
    }
}

void appendByteArray(std::ostringstream& header,
                     std::string_view symbol,
                     const std::vector<std::uint8_t>& bytes) {
    constexpr std::size_t kBytesPerLine = 12;
    constexpr std::array<char, 16> kHexDigits{
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

    header << "inline constexpr std::array<std::uint8_t, " << bytes.size() << "> " << symbol
           << "{\n";
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if ((index % kBytesPerLine) == 0) {
            header << "    ";
        }
        const std::uint8_t byte = bytes[index];
        header << "0x" << kHexDigits[byte >> 4U] << kHexDigits[byte & 0x0FU] << ',';
        if ((index % kBytesPerLine) == (kBytesPerLine - 1) || index + 1 == bytes.size()) {
            header << '\n';
        } else {
            header << ' ';
        }
    }
    header << "};\n\n";
}

[[nodiscard]] std::string makeHeader(const std::vector<std::uint8_t>& composeVertexShader,
                                     const std::vector<std::uint8_t>& blackPixelShader,
                                     const std::vector<std::uint8_t>& nv12PixelShader,
                                     const std::vector<std::uint8_t>& differencePixelShader,
                                     const std::vector<std::uint8_t>& probeVertexShader,
                                     const std::vector<std::uint8_t>& probePixelShader2D,
                                     const std::vector<std::uint8_t>& probePixelShader2DArray) {
    std::ostringstream header;
    header << "// Generated by DvsHlslHeaderCompiler. Do not edit.\n"
              "#pragma once\n\n"
              "#include <array>\n"
              "#include <cstdint>\n\n"
              "namespace dvs::platform::shaders {\n\n";
    appendByteArray(header, "kComposeVertexShader", composeVertexShader);
    appendByteArray(header, "kBlackPixelShader", blackPixelShader);
    appendByteArray(header, "kNv12PixelShader", nv12PixelShader);
    appendByteArray(header, "kDifferencePixelShader", differencePixelShader);
    appendByteArray(header, "kProbeVertexShader", probeVertexShader);
    appendByteArray(header, "kProbePixelShader2D", probePixelShader2D);
    appendByteArray(header, "kProbePixelShader2DArray", probePixelShader2DArray);
    header << "} // namespace dvs::platform::shaders\n";
    return header.str();
}

[[nodiscard]] std::string readFileIfPresent(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return {};
    }
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void writeFileIfChanged(const std::filesystem::path& path, const std::string& contents) {
    if (readFileIfPresent(path) == contents) {
        return;
    }

    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }

    std::filesystem::path temporaryPath = path;
    temporaryPath += L".tmp";
    {
        std::ofstream output{temporaryPath, std::ios::binary | std::ios::trunc};
        if (!output) {
            throw std::runtime_error{"Could not open generated header temporary file '" +
                                     temporaryPath.string() + "'."};
        }
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.close();
        if (!output) {
            std::error_code ignored;
            std::filesystem::remove(temporaryPath, ignored);
            throw std::runtime_error{"Could not write generated header temporary file '" +
                                     temporaryPath.string() + "'."};
        }
    }

    if (!MoveFileExW(temporaryPath.c_str(),
                     path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        std::error_code ignored;
        std::filesystem::remove(temporaryPath, ignored);
        throw std::runtime_error{"Could not publish generated header '" + path.string() +
                                 "' (Win32 error " + std::to_string(error) + ")."};
    }
}

[[nodiscard]] std::wstring_view requireValue(int argc, wchar_t* argv[], int& index) {
    ++index;
    if (index >= argc) {
        throw std::invalid_argument{"Missing value for command-line option."};
    }
    const std::wstring_view value{argv[index]};
    if (value.empty()) {
        throw std::invalid_argument{"Empty value for command-line option."};
    }
    return value;
}

[[nodiscard]] Arguments parseArguments(int argc, wchar_t* argv[]) {
    Arguments arguments;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view option{argv[index]};
        if (option == L"--compose") {
            arguments.composePath = requireValue(argc, argv, index);
        } else if (option == L"--nv12") {
            arguments.nv12Path = requireValue(argc, argv, index);
        } else if (option == L"--probe") {
            arguments.probePath = requireValue(argc, argv, index);
        } else if (option == L"--output") {
            arguments.outputPath = requireValue(argc, argv, index);
        } else {
            throw std::invalid_argument{"Unknown command-line option."};
        }
    }

    if (arguments.composePath.empty() || arguments.nv12Path.empty() ||
        arguments.probePath.empty() || arguments.outputPath.empty()) {
        throw std::invalid_argument{
            "Usage: DvsHlslHeaderCompiler --compose <Compose.hlsl> --nv12 <Nv12ToRgb.hlsl> "
            "--probe <SourcePixelProbe.hlsl> --output <DvsNv12Shaders.generated.h>"};
    }
    return arguments;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        const Arguments arguments = parseArguments(argc, argv);
        const ShaderRequest composeVertex{&arguments.composePath, "ComposeVertexShader", "vs_4_0"};
        const ShaderRequest blackPixel{&arguments.composePath, "BlackPixelShader", "ps_4_0"};
        const ShaderRequest nv12Pixel{&arguments.nv12Path, "Nv12PixelShader", "ps_4_0"};
        const ShaderRequest differencePixel{&arguments.nv12Path, "DifferencePixelShader", "ps_4_0"};
        const ShaderRequest probeVertex{&arguments.probePath, "ProbeVertexShader", "vs_4_0"};
        const ShaderRequest probePixel2D{&arguments.probePath, "ProbePixelShader2D", "ps_4_0"};
        const ShaderRequest probePixel2DArray{
            &arguments.probePath, "ProbePixelShader2DArray", "ps_4_0"};

        const std::vector<std::uint8_t> composeVertexBytecode = compileShader(composeVertex);
        const std::vector<std::uint8_t> blackPixelBytecode = compileShader(blackPixel);
        const std::vector<std::uint8_t> nv12PixelBytecode = compileShader(nv12Pixel);
        const std::vector<std::uint8_t> differencePixelBytecode = compileShader(differencePixel);
        const std::vector<std::uint8_t> probeVertexBytecode = compileShader(probeVertex);
        const std::vector<std::uint8_t> probePixel2DBytecode = compileShader(probePixel2D);
        const std::vector<std::uint8_t> probePixel2DArrayBytecode =
            compileShader(probePixel2DArray);

        constexpr std::array composeSignature{
            ExpectedSignatureParameter{"SV_POSITION", 0U, 0U, 0x0FU},
            ExpectedSignatureParameter{"TEXCOORD", 0U, 1U, 0x01U},
            ExpectedSignatureParameter{"TEXCOORD", 1U, 2U, 0x03U},
        };
        constexpr std::array blackSignature{
            ExpectedSignatureParameter{"SV_POSITION", 0U, 0U, 0x0FU},
            ExpectedSignatureParameter{"TEXCOORD", 0U, 1U, 0x01U},
        };
        constexpr std::array probeVertexSignature{
            ExpectedSignatureParameter{"SV_POSITION", 0U, 0U, 0x0FU},
        };
        constexpr std::array probePixelInputSignature{
            ExpectedSignatureParameter{"SV_POSITION", 0U, 0U, 0x0FU},
        };
        constexpr std::array probePixelOutputSignature{
            // fxc reflects the pixel-shader output semantic normalized as SV_TARGET0.
            ExpectedSignatureParameter{"SV_TARGET", 0U, 0U, 0x0FU},
        };
        validateSignature(
            composeVertexBytecode, composeSignature, true, "ComposeVertexShader output");
        validateSignature(blackPixelBytecode, blackSignature, false, "BlackPixelShader input");
        validateSignature(nv12PixelBytecode, composeSignature, false, "Nv12PixelShader input");
        validateSignature(
            differencePixelBytecode, composeSignature, false, "DifferencePixelShader input");
        validateSignature(
            probeVertexBytecode, probeVertexSignature, true, "ProbeVertexShader output");
        validateSignature(
            probePixel2DBytecode, probePixelInputSignature, false, "ProbePixelShader2D input");
        validateSignature(
            probePixel2DBytecode, probePixelOutputSignature, true, "ProbePixelShader2D output");
        validateSignature(probePixel2DArrayBytecode,
                          probePixelInputSignature,
                          false,
                          "ProbePixelShader2DArray input");
        validateSignature(probePixel2DArrayBytecode,
                          probePixelOutputSignature,
                          true,
                          "ProbePixelShader2DArray output");
        writeFileIfChanged(arguments.outputPath,
                           makeHeader(composeVertexBytecode,
                                      blackPixelBytecode,
                                      nv12PixelBytecode,
                                      differencePixelBytecode,
                                      probeVertexBytecode,
                                      probePixel2DBytecode,
                                      probePixel2DArrayBytecode));
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "DvsHlslHeaderCompiler: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
