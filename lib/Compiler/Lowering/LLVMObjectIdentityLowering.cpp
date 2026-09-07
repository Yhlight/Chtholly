#include "LLVMInternal.h"

#include <iomanip>
#include <sstream>

namespace chtholly::compiler {
namespace {

std::string encodeSymbolPart(std::string_view value) {
  std::ostringstream out;
  out << value.size() << '_';
  out << std::hex << std::setfill('0');
  for (const auto byte : value)
    out << std::setw(2)
        << static_cast<unsigned>(static_cast<unsigned char>(byte));
  return out.str();
}

} // namespace

std::string LLVMObjectIdentityService::mangleFunction(
    std::string_view package, std::string_view module,
    std::string_view function, std::string_view abi_fingerprint) {
  return "__chtholly_next_p" + encodeSymbolPart(package) + "_m" +
         encodeSymbolPart(module) + "_f" + encodeSymbolPart(function) + "_a" +
         std::string(abi_fingerprint);
}

std::string LLVMObjectIdentityService::mangleStatic(
    std::string_view package, std::string_view module, std::string_view name) {
  return "__chtholly_next_p" + encodeSymbolPart(package) + "_m" +
         encodeSymbolPart(module) + "_s" + encodeSymbolPart(name);
}

std::string LLVMObjectIdentityService::coroutineConstructorSymbol(
    const PublicEntity &entity) {
  return "__chtholly_next_coro_ctor_v1_" + entity.fingerprint.hex();
}

} // namespace chtholly::compiler
