// Build-time GLSL compute shader compiler used when a Vulkan SDK executable is
// unavailable. It intentionally has no runtime dependency on the application.
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

class ProcessGuard {
 public:
  ProcessGuard() { glslang::InitializeProcess(); }
  ~ProcessGuard() { glslang::FinalizeProcess(); }
};

bool readText(const char* path, std::string* out) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  *out = std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
  return !out->empty();
}

bool writeSpirv(const char* path, const std::vector<unsigned int>& words) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return false;
  output.write(reinterpret_cast<const char*>(words.data()),
               static_cast<std::streamsize>(words.size() * sizeof(unsigned int)));
  return output.good();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: vic_glslang_compile <input.comp> <output.spv>\n";
    return 2;
  }

  std::string source;
  if (!readText(argv[1], &source)) {
    std::cerr << "cannot read shader: " << argv[1] << '\n';
    return 1;
  }

  ProcessGuard process;
  glslang::TShader shader(EShLangCompute);
  const char* text = source.c_str();
  shader.setStrings(&text, 1);
  shader.setEntryPoint("main");
  shader.setEnvInput(glslang::EShSourceGlsl, EShLangCompute,
                     glslang::EShClientVulkan, 100);
  shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
  shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);

  const auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
  if (!shader.parse(GetDefaultResources(), 450, false, messages)) {
    std::cerr << shader.getInfoLog() << shader.getInfoDebugLog();
    return 1;
  }

  glslang::TProgram program;
  program.addShader(&shader);
  if (!program.link(messages)) {
    std::cerr << program.getInfoLog() << program.getInfoDebugLog();
    return 1;
  }

  std::vector<unsigned int> words;
  glslang::SpvOptions options;
  options.validate = true;
  glslang::GlslangToSpv(*program.getIntermediate(EShLangCompute), words,
                        &options);
  if (words.empty() || !writeSpirv(argv[2], words)) {
    std::cerr << "cannot write SPIR-V: " << argv[2] << '\n';
    return 1;
  }
  return 0;
}
