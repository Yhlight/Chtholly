#include "chtholly_source_fuzz.h"

#include "chtholly/Compiler/CompilationUnit.h"

#include <string>
#include <string_view>

int fuzzNextSource(const std::uint8_t *data, std::size_t size) {
  if (!data || size == 0 || size > 100000)
    return 0;

  const auto mode = data[0] % 3;
  const chtholly::LanguageVersion version{1, (data[0] / 3U) %
      (chtholly::LatestLanguageVersion.minor + 1U)};
  const auto payload = std::string_view(
      reinterpret_cast<const char *>(data + 1), size - 1);
  std::string source;
  if (mode == 0) {
    source.assign(payload);
  } else if (mode == 1) {
    source = "module fuzz;\nfn fuzz(): void {\n";
    source.append(payload);
    source += "\n}\n";
  } else {
    source = "module fuzz;\nfn fuzz(): i32 { return ";
    source.append(payload);
    source += "; }\n";
  }

  chtholly::compiler::CompilationSession session(
      "x86_64-unknown-linux-gnu", "source-fuzz", {},
      chtholly::compiler::defaultCompileToolchainFingerprint(), {},
      {.source = version});
  (void)session.addUnit(
      chtholly::compiler::SourceInput("source-fuzz.cns", std::move(source)));
  chtholly::compiler::CompilationRequest request;
  request.mode = chtholly::compiler::CompilationRequest::Mode::Check;
  std::string error;
  (void)session.compile(error, request);
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data,
                                      std::size_t size) {
  return fuzzNextSource(data, size);
}
