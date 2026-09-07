#include "chtholly/Driver/RegistrySigner.h"

#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

void usage(std::ostream &out) {
  out << "usage: chtholly-registry-signer request --config <signer.toml>\n";
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && (std::string_view(argv[1]) == "--help" ||
                    std::string_view(argv[1]) == "-h")) {
    usage(std::cout);
    return 0;
  }
  if (argc != 4 || std::string_view(argv[1]) != "request" ||
      std::string_view(argv[2]) != "--config" ||
      std::string_view(argv[3]).empty()) {
    usage(std::cerr);
    return 2;
  }
  std::string error;
  auto config = chtholly::loadRegistryFileSignerConfig(argv[3], error);
  const std::string input(std::istreambuf_iterator<char>(std::cin), {});
  auto request = config ? chtholly::parseRegistrySignerRequest(input, error)
                        : std::optional<chtholly::RegistrySignerRequest>{};
  auto provider =
      config ? chtholly::createRegistryFileSigningProvider(std::move(*config))
             : nullptr;
  auto response = request && provider
                      ? provider->execute(*request, error)
                      : std::optional<chtholly::RegistrySignerResponse>{};
  if (!response) {
    std::cerr << "chtholly-registry-signer: " << error << '\n';
    return 1;
  }
  std::cout << chtholly::renderRegistrySignerResponse(*request, *response);
  return std::cout ? 0 : 1;
}
