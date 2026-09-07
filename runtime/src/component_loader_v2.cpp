#include "chtholly/component_loader_v2.h"
#include "chtholly/component_abi_v2.h"
#include "chtholly/Compiler/ComponentABI2Protocol.h"
#include "chtholly/Compiler/ComponentABI2Artifact.h"
#include "chtholly/Compiler/Outcome.h"
#include "chtholly/Support/FileSystem.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

struct chtholly_component_module_v2 {
#if defined(_WIN32)
  HMODULE library = nullptr;
#else
  void *library = nullptr;
#endif
  chtholly::compiler::ComponentAbi2Descriptor descriptor;
  chtholly_next_resource_lease_v2 *lease = nullptr;
  chtholly_component_invoke_fn_v2 invoke = nullptr;
  chtholly_component_close_fn_v2 close = nullptr;
  std::mutex mutex;
  bool closing = false;
  bool closed = false;
  bool close_running = false;
  std::uint64_t active_invocations = 0;
  std::uint64_t owner_references = 0;
};

namespace {
void diag(const char *text, char *out, uint64_t cap, uint64_t *size) {
  const std::string_view value(text ? text : "");
  if (size) *size = value.size();
  if (!out || cap == 0) return;
  const auto n = std::min<uint64_t>(cap - 1, value.size());
  std::memcpy(out, value.data(), static_cast<size_t>(n)); out[n] = 0;
}
void closeLibrary(chtholly_component_module_v2 &module) {
#if defined(_WIN32)
  if (module.library) FreeLibrary(module.library);
#else
  if (module.library) dlclose(module.library);
#endif
  module.library = nullptr;
}
}

extern "C" uint32_t chtholly_component_load_v2(
    const char *path, chtholly_component_module_v2 **out, char *diagnostic,
    uint64_t capacity, uint64_t *size) {
  if (!path || !out) return CHTHOLLY_COMPONENT_LOADER_V2_INVALID_ARGUMENT;
  *out = nullptr;
  try {
    std::error_code ec;
    const auto absolute = std::filesystem::canonical(
        chtholly::pathForFileSystem(path), ec);
    if (ec || !absolute.is_absolute()) {
      diag("ABI-2 component path is not absolute", diagnostic, capacity, size);
      return CHTHOLLY_COMPONENT_LOADER_V2_INVALID_ARGUMENT;
    }
    auto *module = new chtholly_component_module_v2;
#if defined(_WIN32)
    module->library = LoadLibraryExW(absolute.wstring().c_str(), nullptr,
                                     LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    auto query = module->library ? reinterpret_cast<chtholly_component_query_fn_v2>(GetProcAddress(module->library, CHTHOLLY_COMPONENT_QUERY_SYMBOL_V2)) : nullptr;
    module->invoke = module->library ? reinterpret_cast<chtholly_component_invoke_fn_v2>(GetProcAddress(module->library, CHTHOLLY_COMPONENT_INVOKE_SYMBOL_V2)) : nullptr;
    module->close = module->library ? reinterpret_cast<chtholly_component_close_fn_v2>(GetProcAddress(module->library, CHTHOLLY_COMPONENT_CLOSE_SYMBOL_V2)) : nullptr;
#else
    module->library = dlopen(absolute.c_str(), RTLD_NOW | RTLD_LOCAL);
    auto query = module->library ? reinterpret_cast<chtholly_component_query_fn_v2>(dlsym(module->library, CHTHOLLY_COMPONENT_QUERY_SYMBOL_V2)) : nullptr;
    module->invoke = module->library ? reinterpret_cast<chtholly_component_invoke_fn_v2>(dlsym(module->library, CHTHOLLY_COMPONENT_INVOKE_SYMBOL_V2)) : nullptr;
    module->close = module->library ? reinterpret_cast<chtholly_component_close_fn_v2>(dlsym(module->library, CHTHOLLY_COMPONENT_CLOSE_SYMBOL_V2)) : nullptr;
#endif
    if (!module->library || !query || !module->invoke || !module->close) {
      diag("ABI-2 component query/invoke/close symbol is missing", diagnostic, capacity, size);
      closeLibrary(*module); delete module; return CHTHOLLY_COMPONENT_LOADER_V2_QUERY_MISSING;
    }
    const uint8_t *bytes = nullptr; uint64_t byte_count = 0;
    if (query(&bytes, &byte_count) != 0 || !bytes) {
      diag("ABI-2 component query failed", diagnostic, capacity, size);
      closeLibrary(*module); delete module; return CHTHOLLY_COMPONENT_LOADER_V2_DESCRIPTOR_INVALID;
    }
    chtholly::compiler::ComponentAbi2DescriptorError decode_error;
    std::string error;
    auto descriptor = chtholly::compiler::ComponentAbi2Descriptor::decode(
        std::string_view(reinterpret_cast<const char *>(bytes), byte_count), decode_error, error);
    if (!descriptor) {
      diag(error.c_str(), diagnostic, capacity, size); closeLibrary(*module); delete module;
      return decode_error == chtholly::compiler::ComponentAbi2DescriptorError::AbiMismatch
                 ? CHTHOLLY_COMPONENT_LOADER_V2_ABI_MISMATCH
                 : CHTHOLLY_COMPONENT_LOADER_V2_DESCRIPTOR_INVALID;
    }
    module->descriptor = *descriptor;
    const auto digest = module->descriptor.descriptor_digest.bytes();
    if (chtholly_next_resource_lease_v2_create(
            static_cast<uint32_t>(module->descriptor.lease_policy), digest.data(), &module->lease) != 0) {
      diag("ABI-2 resource lease creation failed", diagnostic, capacity, size);
      closeLibrary(*module); delete module; return CHTHOLLY_COMPONENT_LOADER_V2_PLATFORM_ERROR;
    }
    *out = module; diag("", diagnostic, capacity, size); return CHTHOLLY_COMPONENT_LOADER_V2_OK;
  } catch (...) {
    diag("ABI-2 component loader exception", diagnostic, capacity, size);
    return CHTHOLLY_COMPONENT_LOADER_V2_PLATFORM_ERROR;
  }
}

extern "C" uint32_t chtholly_component_load_v2_from_artifact(
    const char *library_path, const char *artifact_path,
    chtholly_component_module_v2 **out, char *diagnostic, uint64_t capacity,
    uint64_t *size) {
  if (!library_path || !artifact_path || !out)
    return CHTHOLLY_COMPONENT_LOADER_V2_INVALID_ARGUMENT;
  chtholly::compiler::ComponentAbi2DescriptorError kind{};
  std::string error;
  const auto expected = chtholly::compiler::readComponentAbi2Artifact(
      artifact_path, kind, error);
  if (!expected) {
    diag(error.c_str(), diagnostic, capacity, size);
    *out = nullptr;
    return CHTHOLLY_COMPONENT_LOADER_V2_DESCRIPTOR_INVALID;
  }
  const auto status = chtholly_component_load_v2(
      library_path, out, diagnostic, capacity, size);
  if (status != CHTHOLLY_COMPONENT_LOADER_V2_OK || !*out)
    return status;
  if ((*out)->descriptor.descriptor_digest != expected->descriptor_digest) {
    diag("provider descriptor differs from replayed artifact", diagnostic,
         capacity, size);
    (void)chtholly_component_unload_v2(*out, nullptr, 0, nullptr);
    *out = nullptr;
    return CHTHOLLY_COMPONENT_LOADER_V2_DESCRIPTOR_MISMATCH;
  }
  return CHTHOLLY_COMPONENT_LOADER_V2_OK;
}

extern "C" uint32_t chtholly_component_invoke_v2(
    chtholly_component_module_v2 *module, uint8_t operation_kind,
    chtholly_next_resource_operation_v2 **out_operation, char *diagnostic,
    uint64_t capacity, uint64_t *size) {
  return chtholly_component_invoke_payload_v2(module, operation_kind, nullptr,
                                               out_operation, diagnostic,
                                               capacity, size);
}

extern "C" uint32_t chtholly_component_invoke_payload_v2(
    chtholly_component_module_v2 *module, uint8_t operation_kind,
    chtholly_next_payload_transport_v2 *transport,
    chtholly_next_resource_operation_v2 **out_operation, char *diagnostic,
    uint64_t capacity, uint64_t *size) {
  if (!module || !out_operation) return CHTHOLLY_COMPONENT_LOADER_V2_INVALID_ARGUMENT;
  *out_operation = nullptr; std::unique_lock lock(module->mutex);
  if (module->closing || module->closed) return CHTHOLLY_COMPONENT_LOADER_V2_CLOSING;
  if (transport && operation_kind !=
      static_cast<uint8_t>(module->descriptor.operation_kind)) {
    diag("ABI-2 payload operation kind differs from provider descriptor",
         diagnostic, capacity, size);
    return CHTHOLLY_COMPONENT_LOADER_V2_DESCRIPTOR_MISMATCH;
  }
  const auto digest = module->descriptor.descriptor_digest.bytes();
  if (transport && chtholly_next_payload_transport_v2_check_digest(
                       transport, digest.data()) != CHTHOLLY_NEXT_RESOURCE_LEASE_OK) {
    diag("ABI-2 payload transport descriptor digest mismatch", diagnostic,
         capacity, size);
    return CHTHOLLY_COMPONENT_LOADER_V2_DESCRIPTOR_MISMATCH;
  }
  if (chtholly_next_resource_operation_v2_begin(module->lease, digest.data(), out_operation) != 0)
    return CHTHOLLY_COMPONENT_LOADER_V2_NOT_READY;
  chtholly_component_invocation_v2 invocation{};
  invocation.struct_size = sizeof(invocation);
  invocation.abi_epoch = CHTHOLLY_COMPONENT_ABI_EPOCH_V2;
  invocation.operation_kind = operation_kind;
  std::memcpy(invocation.descriptor_digest, digest.data(), digest.size());
  const auto copy_fingerprint = [](uint8_t *out,
                                    const chtholly::compiler::StableFingerprint &value) {
    const auto bytes = value.bytes();
    std::memcpy(out, bytes.data(), bytes.size());
  };
  copy_fingerprint(invocation.payload_type_digest,
                   module->descriptor.payload_type_digest);
  copy_fingerprint(invocation.layout_digest, module->descriptor.layout_digest);
  copy_fingerprint(invocation.lifecycle_digest,
                   module->descriptor.lifecycle_digest);
  copy_fingerprint(invocation.contract_digest,
                   module->descriptor.contract_digest);
  const auto plan_fingerprint = chtholly::compiler::componentAbi2PayloadPlanDigest(
      module->descriptor,
      chtholly::compiler::makeChannelOutcome().fingerprint(), 0, 1, 0, true,
      operation_kind == static_cast<uint8_t>(chtholly::compiler::ComponentAbi2OperationKind::Receive));
  copy_fingerprint(invocation.plan_fingerprint, plan_fingerprint);
  invocation.payload_size = 0;
  invocation.source_lane = 0;
  invocation.destination_lane = 1;
  invocation.token_lane = 0;
  invocation.payload_size = transport
      ? chtholly_next_payload_transport_v2_payload_size(transport) : 0;
  invocation.operation = *out_operation;
  invocation.transport = transport;
  ++module->active_invocations;
  lock.unlock();
  const auto invoke_status = module->invoke(&invocation);
  lock.lock();
  --module->active_invocations;
  if (invoke_status != 0) {
    chtholly_next_resource_operation_v2_complete(*out_operation, CHTHOLLY_NEXT_RESOURCE_OPERATION_FAILED);
    chtholly_next_resource_operation_v2_destroy(*out_operation); *out_operation = nullptr;
    diag("ABI-2 component operation failed", diagnostic, capacity, size);
    return CHTHOLLY_COMPONENT_LOADER_V2_OPERATION_FAILED;
  }
  return CHTHOLLY_COMPONENT_LOADER_V2_OK;
}

extern "C" uint32_t chtholly_component_close_v2(
    chtholly_component_module_v2 *module, char *diagnostic, uint64_t capacity, uint64_t *size) {
  if (!module) return CHTHOLLY_COMPONENT_LOADER_V2_INVALID_ARGUMENT;
  std::unique_lock lock(module->mutex);
  if (module->closed) return CHTHOLLY_COMPONENT_LOADER_V2_OK;
  module->closing = true;
  (void)chtholly_next_resource_lease_v2_begin_close(module->lease);
  if (module->active_invocations || module->close_running) return CHTHOLLY_COMPONENT_LOADER_V2_NOT_READY;
  module->close_running = true;
  lock.unlock();
  // Provider close requests cancellation and joins background users. It must
  // run before quiescence, otherwise pending operations could never drain.
  const auto provider_status = module->close();
  lock.lock();
  module->close_running = false;
  if (provider_status != 0) return CHTHOLLY_COMPONENT_LOADER_V2_PLATFORM_ERROR;
  if (module->owner_references || chtholly_next_resource_lease_v2_quiesce(module->lease) != 0) {
    diag("ABI-2 component is waiting for resource/callback quiescence", diagnostic, capacity, size);
    return CHTHOLLY_COMPONENT_LOADER_V2_NOT_READY;
  }
  if (chtholly_next_resource_lease_v2_destroy(module->lease) != 0)
    return CHTHOLLY_COMPONENT_LOADER_V2_NOT_READY;
  module->lease = nullptr;
  closeLibrary(*module);
  module->closed = true;
  diag("", diagnostic, capacity, size);
  return CHTHOLLY_COMPONENT_LOADER_V2_OK;
}
extern "C" int32_t chtholly_component_retain_owner_v2(void *opaque) {
  auto *module = static_cast<chtholly_component_module_v2 *>(opaque);
  if (!module) return CHTHOLLY_NEXT_RESOURCE_LEASE_INVALID_HANDLE;
  std::lock_guard lock(module->mutex);
  if (module->closed || (module->closing && module->owner_references == 0))
    return CHTHOLLY_NEXT_RESOURCE_LEASE_CLOSED;
  ++module->owner_references;
  return 0;
}
extern "C" void chtholly_component_release_owner_v2(void *opaque) {
  auto *module = static_cast<chtholly_component_module_v2 *>(opaque);
  if (!module) return;
  std::lock_guard lock(module->mutex);
  if (module->owner_references) --module->owner_references;
}
extern "C" uint32_t chtholly_component_release_v2(chtholly_component_module_v2 *module) {
  if (!module || !module->closed) return CHTHOLLY_COMPONENT_LOADER_V2_CLOSING;
  delete module; return CHTHOLLY_COMPONENT_LOADER_V2_OK;
}
extern "C" uint32_t chtholly_component_unload_v2(
    chtholly_component_module_v2 *module, char *diagnostic, uint64_t capacity, uint64_t *size) {
  const auto status = chtholly_component_close_v2(module, diagnostic, capacity, size);
  return status == CHTHOLLY_COMPONENT_LOADER_V2_OK ? chtholly_component_release_v2(module) : status;
}
