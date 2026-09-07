#include "chtholly/component_loader_v1.h"

#include "chtholly/Support/Digest.h"
#include "chtholly/Support/FileSystem.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <string_view>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

struct chtholly_component_module_v1 {
#if defined(_WIN32)
  HMODULE library = nullptr;
#else
  void *library = nullptr;
#endif
  const chtholly_component_descriptor_v1 *descriptor = nullptr;
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::uint64_t active_calls = 0;
  bool closing = false;
  bool closed = false;
};

static_assert(sizeof(chtholly_component_value_v1) == 32);
static_assert(sizeof(chtholly_component_export_descriptor_v1) == 112);
static_assert(sizeof(chtholly_component_descriptor_v1) == 208);

namespace {

void diagnostic(std::string_view message, char *output, std::uint64_t capacity,
                std::uint64_t *size) {
  if (size)
    *size = message.size();
  if (!output || capacity == 0)
    return;
  const auto count = std::min<std::uint64_t>(capacity - 1, message.size());
  if (count != 0)
    std::memcpy(output, message.data(), static_cast<std::size_t>(count));
  output[count] = 0;
}

bool zero(std::span<const std::uint8_t> bytes) {
  return std::ranges::all_of(bytes, [](auto byte) { return byte == 0; });
}

bool validUtf8(std::string_view value, bool ascii_only = false) {
  if (value.empty() || value.size() > 4096 ||
      std::ranges::any_of(value, [](char byte) {
        return byte == '\0' || byte == '\t' || byte == '\r' || byte == '\n';
      }))
    return false;
  std::size_t index = 0;
  while (index < value.size()) {
    const auto first = static_cast<std::uint8_t>(value[index]);
    if (first < 0x80) {
      if (first < 0x20 || first == 0x7f)
        return false;
      ++index;
      continue;
    }
    if (ascii_only)
      return false;
    std::size_t count = 0;
    std::uint32_t codepoint = 0;
    if ((first & 0xe0) == 0xc0) {
      count = 2;
      codepoint = first & 0x1f;
    } else if ((first & 0xf0) == 0xe0) {
      count = 3;
      codepoint = first & 0x0f;
    } else if ((first & 0xf8) == 0xf0) {
      count = 4;
      codepoint = first & 0x07;
    } else {
      return false;
    }
    if (index + count > value.size())
      return false;
    for (std::size_t offset = 1; offset < count; ++offset) {
      const auto next = static_cast<std::uint8_t>(value[index + offset]);
      if ((next & 0xc0) != 0x80)
        return false;
      codepoint = (codepoint << 6) | (next & 0x3f);
    }
    if ((count == 2 && codepoint < 0x80) || (count == 3 && codepoint < 0x800) ||
        (count == 4 && codepoint < 0x10000) || codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff))
      return false;
    index += count;
  }
  return true;
}

std::array<std::uint8_t, 32> digestBytes(std::string_view canonical) {
  const auto hex = chtholly::sha256Hex(canonical);
  std::array<std::uint8_t, 32> result{};
  for (std::size_t index = 0; index < result.size(); ++index) {
    unsigned value = 0;
    const auto part = hex.substr(index * 2, 2);
    std::from_chars(part.data(), part.data() + part.size(), value, 16);
    result[index] = static_cast<std::uint8_t>(value);
  }
  return result;
}

std::string hex(std::span<const std::uint8_t> bytes) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result(bytes.size() * 2, '0');
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    result[index * 2] = digits[bytes[index] >> 4];
    result[index * 2 + 1] = digits[bytes[index] & 15];
  }
  return result;
}

bool equalDigest(const std::uint8_t *left, const std::uint8_t *right) {
  return std::memcmp(left, right, 32) == 0;
}

bool verifyDescriptor(const chtholly_component_descriptor_v1 &descriptor,
                      std::string &error) {
  if (descriptor.struct_size != sizeof(descriptor) ||
      descriptor.abi_epoch != CHTHOLLY_COMPONENT_ABI_EPOCH_V1 ||
      descriptor.flags != 0 || descriptor.reserved != 0 ||
      !descriptor.identity || descriptor.identity_size == 0 ||
      descriptor.identity_size > 4096 || !descriptor.exports ||
      descriptor.export_count == 0 || descriptor.export_count > 1024 ||
      std::ranges::any_of(descriptor.reserved_words,
                          [](auto value) { return value != 0; })) {
    error = "component descriptor header is invalid";
    return false;
  }
  const std::string_view identity(descriptor.identity,
                                  descriptor.identity_size);
  const auto identity_digest =
      digestBytes("chtholly.component.identity.v1\n" + std::string(identity));
  if (!equalDigest(identity_digest.data(), descriptor.identity_digest)) {
    error = "component descriptor identity digest is invalid";
    return false;
  }
  std::string canonical =
      "chtholly.component.contract.v1\n" + std::string(identity) + "\n";
  std::array<std::uint8_t, 32> previous{};
  bool have_previous = false;
  for (std::uint64_t index = 0; index < descriptor.export_count; ++index) {
    const auto &entry = descriptor.exports[index];
    if (entry.struct_size != sizeof(entry) || entry.flags != 0 ||
        !entry.canonical_name || entry.canonical_name_size == 0 ||
        entry.canonical_name_size > 4096 || !entry.invoke ||
        std::ranges::any_of(entry.reserved,
                            [](auto value) { return value != 0; }) ||
        (have_previous &&
         std::memcmp(previous.data(), entry.export_id, 32) >= 0)) {
      error = "component export descriptor is invalid or unsorted";
      return false;
    }
    std::copy_n(entry.export_id, 32, previous.begin());
    have_previous = true;
    canonical += hex({entry.export_id, 32}) + "\n" +
                 hex({entry.signature_digest, 32}) + "\n" +
                 std::string(entry.canonical_name, entry.canonical_name_size) +
                 "\n";
  }
  const auto contract = digestBytes(canonical);
  if (!equalDigest(contract.data(), descriptor.contract_digest)) {
    error = "component descriptor contract digest is invalid";
    return false;
  }
  return true;
}

bool closeLibrary(chtholly_component_module_v1 &module) {
  bool closed = true;
#if defined(_WIN32)
  if (module.library)
    closed = FreeLibrary(module.library) != 0;
#else
  if (module.library)
    closed = dlclose(module.library) == 0;
#endif
  if (closed)
    module.library = nullptr;
  return closed;
}

} // namespace

extern "C" uint32_t chtholly_component_requirement_init_v1(
    const char *identity, uint64_t identity_size,
    const uint8_t contract_digest[32], const char *target, uint64_t target_size,
    const char *runtime_abi, uint64_t runtime_abi_size,
    chtholly_component_requirement_v1 *out_requirement, char *output,
    uint64_t capacity, uint64_t *output_size) {
  try {
    if (out_requirement)
      std::memset(out_requirement, 0, sizeof(*out_requirement));
    if (!identity || !contract_digest || !target || !runtime_abi ||
        !out_requirement || identity_size == 0 || identity_size > 4096 ||
        target_size == 0 || target_size > 4096 || runtime_abi_size == 0 ||
        runtime_abi_size > 4096 ||
        !validUtf8({identity, static_cast<std::size_t>(identity_size)}) ||
        !validUtf8({target, static_cast<std::size_t>(target_size)}, true) ||
        !validUtf8({runtime_abi, static_cast<std::size_t>(runtime_abi_size)},
                   true) ||
        zero({contract_digest, 32})) {
      diagnostic("invalid component requirement inputs", output, capacity,
                 output_size);
      return CHTHOLLY_COMPONENT_LOADER_INVALID_ARGUMENT_V1;
    }
    const std::string_view target_view(target,
                                       static_cast<std::size_t>(target_size));
    const std::string_view runtime_view(
        runtime_abi, static_cast<std::size_t>(runtime_abi_size));
    if ((target_view != "x86_64-pc-windows-msvc" &&
         target_view != "x86_64-unknown-linux-gnu") ||
        runtime_view != "v1") {
      diagnostic("unsupported component target or runtime ABI", output,
                 capacity, output_size);
      return CHTHOLLY_COMPONENT_LOADER_INVALID_ARGUMENT_V1;
    }
    out_requirement->struct_size = sizeof(*out_requirement);
    out_requirement->abi_epoch = CHTHOLLY_COMPONENT_ABI_EPOCH_V1;
    const auto identity_digest = digestBytes(
        "chtholly.component.identity.v1\n" +
        std::string(identity, static_cast<std::size_t>(identity_size)));
    const auto target_digest = digestBytes("chtholly.component.target.v1\n" +
                                           std::string(target_view));
    const auto runtime_digest = digestBytes("chtholly.component.runtime.v1\n" +
                                            std::string(runtime_view));
    std::ranges::copy(identity_digest, out_requirement->identity_digest);
    std::copy_n(contract_digest, 32, out_requirement->contract_digest);
    std::ranges::copy(target_digest, out_requirement->target_digest);
    std::ranges::copy(runtime_digest, out_requirement->runtime_abi_digest);
    diagnostic({}, output, capacity, output_size);
    return CHTHOLLY_COMPONENT_LOADER_OK_V1;
  } catch (...) {
    if (out_requirement)
      std::memset(out_requirement, 0, sizeof(*out_requirement));
    diagnostic("component requirement initialization raised an exception",
               output, capacity, output_size);
    return CHTHOLLY_COMPONENT_LOADER_PLATFORM_ERROR_V1;
  }
}

extern "C" uint32_t chtholly_component_load_v1(
    const char *path, const chtholly_component_requirement_v1 *requirement,
    chtholly_component_module_v1 **out_module, char *output, uint64_t capacity,
    uint64_t *output_size) {
  chtholly_component_module_v1 *module = nullptr;
  try {
    if (out_module)
      *out_module = nullptr;
    if (!path || !requirement || !out_module ||
        requirement->struct_size != sizeof(*requirement) ||
        std::ranges::any_of(requirement->reserved,
                            [](auto value) { return value != 0; }) ||
        zero({requirement->identity_digest, 32}) ||
        zero({requirement->contract_digest, 32}) ||
        zero({requirement->target_digest, 32}) ||
        zero({requirement->runtime_abi_digest, 32})) {
      diagnostic("invalid component load arguments", output, capacity,
                 output_size);
      return CHTHOLLY_COMPONENT_LOADER_INVALID_ARGUMENT_V1;
    }
    if (requirement->abi_epoch != CHTHOLLY_COMPONENT_ABI_EPOCH_V1) {
      diagnostic("component requirement ABI epoch mismatch", output, capacity,
                 output_size);
      return CHTHOLLY_COMPONENT_LOADER_ABI_MISMATCH_V1;
    }
    const auto input_path = chtholly::pathForFileSystem(path);
#if defined(_WIN32)
    const std::string_view path_text(path);
    const bool input_absolute =
        (path_text.size() >= 3 &&
         ((path_text[0] >= 'A' && path_text[0] <= 'Z') ||
          (path_text[0] >= 'a' && path_text[0] <= 'z')) &&
         path_text[1] == ':' &&
         (path_text[2] == '\\' || path_text[2] == '/')) ||
        path_text.starts_with("\\\\") || path_text.starts_with("//");
#else
    const bool input_absolute = input_path.is_absolute();
#endif
    std::error_code path_error;
    const auto absolute = std::filesystem::canonical(input_path, path_error);
    if (!input_absolute || path_error || !absolute.is_absolute()) {
      diagnostic("component path is not absolute", output, capacity,
                 output_size);
      return CHTHOLLY_COMPONENT_LOADER_INVALID_ARGUMENT_V1;
    }
    module = new (std::nothrow) chtholly_component_module_v1;
    if (!module) {
      diagnostic("component loader allocation failed", output, capacity,
                 output_size);
      return CHTHOLLY_COMPONENT_LOADER_PLATFORM_ERROR_V1;
    }
    chtholly_component_query_fn_v1 query = nullptr;
#if defined(_WIN32)
    const auto wide = absolute.wstring();
    module->library = LoadLibraryExW(wide.c_str(), nullptr,
                                     LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                         LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (module->library)
      query = reinterpret_cast<chtholly_component_query_fn_v1>(
          GetProcAddress(module->library, CHTHOLLY_COMPONENT_QUERY_SYMBOL_V1));
#else
    module->library = dlopen(absolute.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (module->library)
      query = reinterpret_cast<chtholly_component_query_fn_v1>(
          dlsym(module->library, CHTHOLLY_COMPONENT_QUERY_SYMBOL_V1));
#endif
    if (!module->library || !query) {
      const auto status = module->library
                              ? CHTHOLLY_COMPONENT_LOADER_QUERY_MISSING_V1
                              : CHTHOLLY_COMPONENT_LOADER_PLATFORM_ERROR_V1;
      diagnostic(module->library ? "component query symbol is missing"
                                 : "platform component load failed",
                 output, capacity, output_size);
      (void)closeLibrary(*module);
      delete module;
      return status;
    }
    module->descriptor = query();
    if (module->descriptor &&
        module->descriptor->abi_epoch != CHTHOLLY_COMPONENT_ABI_EPOCH_V1) {
      diagnostic(module->descriptor->abi_epoch == 2
                     ? "ABI-1 loader rejects ABI-2 component descriptor"
                     : "component descriptor ABI epoch mismatch",
                 output, capacity, output_size);
      (void)closeLibrary(*module);
      delete module;
      return CHTHOLLY_COMPONENT_LOADER_ABI_MISMATCH_V1;
    }
    std::string verify_error;
    if (!module->descriptor ||
        !verifyDescriptor(*module->descriptor, verify_error)) {
      diagnostic(verify_error.empty() ? "component query returned null"
                                      : verify_error,
                 output, capacity, output_size);
      (void)closeLibrary(*module);
      delete module;
      return CHTHOLLY_COMPONENT_LOADER_DESCRIPTOR_INVALID_V1;
    }
    const auto &descriptor = *module->descriptor;
    const struct Match {
      const std::uint8_t *actual;
      const std::uint8_t *expected;
      std::uint32_t status;
      const char *message;
    } matches[] = {{descriptor.identity_digest, requirement->identity_digest,
                    CHTHOLLY_COMPONENT_LOADER_IDENTITY_MISMATCH_V1,
                    "component identity mismatch"},
                   {descriptor.contract_digest, requirement->contract_digest,
                    CHTHOLLY_COMPONENT_LOADER_CONTRACT_MISMATCH_V1,
                    "component contract mismatch"},
                   {descriptor.target_digest, requirement->target_digest,
                    CHTHOLLY_COMPONENT_LOADER_TARGET_MISMATCH_V1,
                    "component target mismatch"},
                   {descriptor.runtime_abi_digest,
                    requirement->runtime_abi_digest,
                    CHTHOLLY_COMPONENT_LOADER_RUNTIME_MISMATCH_V1,
                    "component runtime ABI mismatch"}};
    for (const auto &match : matches) {
      if (equalDigest(match.actual, match.expected))
        continue;
      diagnostic(match.message, output, capacity, output_size);
      (void)closeLibrary(*module);
      delete module;
      return match.status;
    }
    *out_module = module;
    diagnostic({}, output, capacity, output_size);
    return CHTHOLLY_COMPONENT_LOADER_OK_V1;
  } catch (...) {
    if (module) {
      (void)closeLibrary(*module);
      delete module;
    }
    if (out_module)
      *out_module = nullptr;
    diagnostic("component loader raised an internal exception", output,
               capacity, output_size);
    return CHTHOLLY_COMPONENT_LOADER_PLATFORM_ERROR_V1;
  }
}

extern "C" uint32_t
chtholly_component_export_count_v1(const chtholly_component_module_v1 *module,
                                   uint64_t *out_count) {
  try {
    if (!module || !out_count)
      return CHTHOLLY_COMPONENT_LOADER_INVALID_ARGUMENT_V1;
    std::lock_guard lock(module->mutex);
    if (module->closing)
      return CHTHOLLY_COMPONENT_LOADER_CLOSING_V1;
    *out_count = module->descriptor->export_count;
    return CHTHOLLY_COMPONENT_LOADER_OK_V1;
  } catch (...) {
    return CHTHOLLY_COMPONENT_LOADER_PLATFORM_ERROR_V1;
  }
}

extern "C" uint32_t chtholly_component_export_info_v1_get(
    const chtholly_component_module_v1 *module, uint64_t index,
    chtholly_component_export_info_v1 *out_info, char *name,
    uint64_t name_capacity, uint64_t *name_size) {
  try {
    if (!module || !out_info || out_info->struct_size != sizeof(*out_info))
      return CHTHOLLY_COMPONENT_LOADER_INVALID_ARGUMENT_V1;
    std::lock_guard lock(module->mutex);
    if (module->closing)
      return CHTHOLLY_COMPONENT_LOADER_CLOSING_V1;
    if (index >= module->descriptor->export_count)
      return CHTHOLLY_COMPONENT_LOADER_EXPORT_NOT_FOUND_V1;
    const auto &entry = module->descriptor->exports[index];
    out_info->flags = entry.flags;
    std::copy_n(entry.export_id, 32, out_info->export_id);
    std::copy_n(entry.signature_digest, 32, out_info->signature_digest);
    out_info->canonical_name_size = entry.canonical_name_size;
    if (name_size)
      *name_size = entry.canonical_name_size;
    if (!name || name_capacity <= entry.canonical_name_size)
      return CHTHOLLY_COMPONENT_LOADER_BUFFER_TOO_SMALL_V1;
    std::memcpy(name, entry.canonical_name, entry.canonical_name_size);
    name[entry.canonical_name_size] = 0;
    return CHTHOLLY_COMPONENT_LOADER_OK_V1;
  } catch (...) {
    return CHTHOLLY_COMPONENT_LOADER_PLATFORM_ERROR_V1;
  }
}

extern "C" uint32_t chtholly_component_invoke_v1(
    chtholly_component_module_v1 *module, const uint8_t export_id[32],
    const chtholly_component_value_v1 *arguments, uint32_t argument_count,
    chtholly_component_value_v1 *result, char *output, uint64_t capacity,
    uint64_t *output_size) {
  try {
    if (!module || !export_id || !result) {
      diagnostic("invalid component invoke arguments", output, capacity,
                 output_size);
      return CHTHOLLY_COMPONENT_LOADER_INVALID_ARGUMENT_V1;
    }
    const chtholly_component_export_descriptor_v1 *entry = nullptr;
    {
      std::lock_guard lock(module->mutex);
      if (module->closing)
        return CHTHOLLY_COMPONENT_LOADER_CLOSING_V1;
      for (std::uint64_t index = 0; index < module->descriptor->export_count;
           ++index)
        if (equalDigest(module->descriptor->exports[index].export_id,
                        export_id)) {
          entry = &module->descriptor->exports[index];
          break;
        }
      if (!entry)
        return CHTHOLLY_COMPONENT_LOADER_EXPORT_NOT_FOUND_V1;
      ++module->active_calls;
    }
    const auto invoke_status = entry->invoke(arguments, argument_count, result);
    {
      std::lock_guard lock(module->mutex);
      --module->active_calls;
      if (module->active_calls == 0)
        module->changed.notify_all();
    }
    if (invoke_status != CHTHOLLY_COMPONENT_INVOKE_OK_V1) {
      diagnostic("component export rejected its value arguments", output,
                 capacity, output_size);
      return CHTHOLLY_COMPONENT_LOADER_INVOKE_FAILED_V1;
    }
    diagnostic({}, output, capacity, output_size);
    return CHTHOLLY_COMPONENT_LOADER_OK_V1;
  } catch (...) {
    diagnostic("component invocation raised a loader exception", output,
               capacity, output_size);
    return CHTHOLLY_COMPONENT_LOADER_PLATFORM_ERROR_V1;
  }
}

extern "C" uint32_t
chtholly_component_close_v1(chtholly_component_module_v1 *module, char *output,
                            uint64_t capacity, uint64_t *output_size) {
  try {
    if (!module)
      return CHTHOLLY_COMPONENT_LOADER_INVALID_ARGUMENT_V1;
    {
      std::unique_lock lock(module->mutex);
      module->closing = true;
      module->changed.wait(lock, [&] { return module->active_calls == 0; });
      if (!module->closed) {
        if (!closeLibrary(*module)) {
          diagnostic("platform component unload failed", output, capacity,
                     output_size);
          return CHTHOLLY_COMPONENT_LOADER_PLATFORM_ERROR_V1;
        }
        module->descriptor = nullptr;
        module->closed = true;
      }
    }
    diagnostic({}, output, capacity, output_size);
    return CHTHOLLY_COMPONENT_LOADER_OK_V1;
  } catch (...) {
    diagnostic("component close raised a loader exception", output, capacity,
               output_size);
    return CHTHOLLY_COMPONENT_LOADER_PLATFORM_ERROR_V1;
  }
}

extern "C" uint32_t
chtholly_component_release_v1(chtholly_component_module_v1 *module) {
  try {
    if (!module)
      return CHTHOLLY_COMPONENT_LOADER_INVALID_ARGUMENT_V1;
    {
      std::lock_guard lock(module->mutex);
      if (!module->closed || module->active_calls != 0)
        return CHTHOLLY_COMPONENT_LOADER_CLOSING_V1;
    }
    delete module;
    return CHTHOLLY_COMPONENT_LOADER_OK_V1;
  } catch (...) {
    return CHTHOLLY_COMPONENT_LOADER_PLATFORM_ERROR_V1;
  }
}

extern "C" uint32_t
chtholly_component_unload_v1(chtholly_component_module_v1 *module, char *output,
                             uint64_t capacity, uint64_t *output_size) {
  const auto closed =
      chtholly_component_close_v1(module, output, capacity, output_size);
  if (closed != CHTHOLLY_COMPONENT_LOADER_OK_V1)
    return closed;
  return chtholly_component_release_v1(module);
}
