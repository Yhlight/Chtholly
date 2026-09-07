#include "chtholly/component_abi_v2.h"
#include "chtholly/Compiler/ComponentABI2Protocol.h"
#include "chtholly/Compiler/Outcome.h"

#include <string>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>
#include <atomic>

namespace {
std::mutex workers_mutex;
std::vector<std::thread> workers;
std::vector<chtholly_next_resource_operation_v2 *> pending_operations;
std::atomic<unsigned> close_attempts{0};
std::string mode() {
  const char *value = std::getenv("CHTHOLLY_ABI2_FIXTURE_MODE");
  return value ? value : "commit";
}
std::string descriptor_bytes() {
  chtholly::compiler::ComponentAbi2Descriptor descriptor;
  descriptor.component_identity = "abi2.pending.fixture";
  descriptor.entity_identity = "abi2.pending.invoke";
  descriptor.resource_identity = "abi2.pending.resource";
  descriptor.operation_kind = chtholly::compiler::ComponentAbi2OperationKind::Send;
  descriptor.terminal_cardinality = chtholly::compiler::ComponentAbi2TerminalCardinality::OneShot;
  descriptor.lease_policy = chtholly::compiler::ComponentAbi2LeasePolicy::Shared;
  descriptor.payload_type_digest = chtholly::compiler::StableFingerprint::fromCanonicalBytes("payload");
  descriptor.layout_digest = chtholly::compiler::StableFingerprint::fromCanonicalBytes("layout");
  descriptor.lifecycle_digest = chtholly::compiler::StableFingerprint::fromCanonicalBytes("lifecycle");
  descriptor.contract_digest = chtholly::compiler::StableFingerprint::fromCanonicalBytes("contract");
  descriptor.runtime_abi_digest = chtholly::compiler::StableFingerprint::fromCanonicalBytes("runtime-v2");
  std::string error;
  return descriptor.encode(error);
}
} // namespace

#if defined(_WIN32)
#define ABI2_EXPORT __declspec(dllexport)
#else
#define ABI2_EXPORT __attribute__((visibility("default")))
#endif

extern "C" ABI2_EXPORT int32_t chtholly_component_query_v2(const uint8_t **bytes,
                                                uint64_t *size) {
  static const std::string descriptor = descriptor_bytes();
  if (!bytes || !size) return -1;
  *bytes = reinterpret_cast<const uint8_t *>(descriptor.data());
  *size = descriptor.size();
  return 0;
}

extern "C" ABI2_EXPORT int32_t chtholly_component_invoke_v2(
    const chtholly_component_invocation_v2 *invocation) {
  if (!invocation || invocation->struct_size < sizeof(*invocation) ||
      invocation->abi_epoch != CHTHOLLY_COMPONENT_ABI_EPOCH_V2)
    return -1;
  const uint8_t operation_kind = invocation->operation_kind;
  const auto behavior = mode();
  chtholly_next_resource_operation_v2 *operation = invocation->operation;
  if (invocation->transport) {
    chtholly::compiler::ComponentAbi2Descriptor descriptor;
    descriptor.component_identity = "abi2.pending.fixture";
    descriptor.entity_identity = "abi2.pending.invoke";
    descriptor.resource_identity = "abi2.pending.resource";
    descriptor.operation_kind = chtholly::compiler::ComponentAbi2OperationKind::Send;
    descriptor.terminal_cardinality = chtholly::compiler::ComponentAbi2TerminalCardinality::OneShot;
    descriptor.lease_policy = chtholly::compiler::ComponentAbi2LeasePolicy::Shared;
    descriptor.payload_type_digest = chtholly::compiler::StableFingerprint::fromCanonicalBytes("payload");
    descriptor.layout_digest = chtholly::compiler::StableFingerprint::fromCanonicalBytes("layout");
    descriptor.lifecycle_digest = chtholly::compiler::StableFingerprint::fromCanonicalBytes("lifecycle");
    descriptor.contract_digest = chtholly::compiler::StableFingerprint::fromCanonicalBytes("contract");
    descriptor.runtime_abi_digest = chtholly::compiler::StableFingerprint::fromCanonicalBytes("runtime-v2");
    std::string descriptor_error;
    if (descriptor.encode(descriptor_error).empty() ||
        !descriptor.canonicalize(descriptor_error) ||
        operation_kind != static_cast<uint8_t>(descriptor.operation_kind) ||
        std::memcmp(invocation->payload_type_digest,
                     descriptor.payload_type_digest.bytes().data(), 32) != 0 ||
        std::memcmp(invocation->layout_digest,
                     descriptor.layout_digest.bytes().data(), 32) != 0 ||
        std::memcmp(invocation->lifecycle_digest,
                     descriptor.lifecycle_digest.bytes().data(), 32) != 0 ||
        std::memcmp(invocation->contract_digest,
                     descriptor.contract_digest.bytes().data(), 32) != 0)
      return -1;
    const auto plan_fingerprint =
        chtholly::compiler::componentAbi2PayloadPlanDigest(
            descriptor, chtholly::compiler::makeChannelOutcome().fingerprint(),
            0, 1, 0, true, false);
    if (std::memcmp(invocation->descriptor_digest,
                    descriptor.descriptor_digest.bytes().data(), 32) != 0 ||
        std::memcmp(invocation->plan_fingerprint,
                    plan_fingerprint.bytes().data(), 32) != 0 ||
        invocation->payload_size != 4 || invocation->reserved_tail != 0)
      return -1;
    static const uint8_t payload[4] = {4, 3, 2, 1};
    void *token = nullptr;
    if (chtholly_next_payload_transport_v2_send_prepare(
            invocation->transport, payload, sizeof(payload), &token) != 0)
      return -1;
    const auto payload_status = behavior == "cancel"
        ? chtholly_next_payload_transport_v2_send_cancel(token)
        : behavior == "fail" ? chtholly_next_payload_transport_v2_send_fail(token)
                             : chtholly_next_payload_transport_v2_send_commit(token);
    if (payload_status != 0) return -1;
  }
  if (!operation) return -1;
  if (behavior == "pending") return 0;
  if (behavior == "background") {
    if (chtholly_next_resource_operation_v2_provider_enter(operation) != 0) return -1;
    std::lock_guard lock(workers_mutex);
    pending_operations.push_back(operation);
    workers.emplace_back([operation] {
      while (!chtholly_next_resource_operation_v2_cancel_requested(operation))
        std::this_thread::yield();
      (void)chtholly_next_resource_operation_v2_complete(operation, CHTHOLLY_NEXT_RESOURCE_OPERATION_CANCELLED);
      (void)chtholly_next_resource_operation_v2_provider_leave(operation);
    });
    return 0;
  }
  const uint32_t terminal = behavior == "cancel" ? CHTHOLLY_NEXT_RESOURCE_OPERATION_CANCELLED
      : behavior == "fail" ? CHTHOLLY_NEXT_RESOURCE_OPERATION_FAILED
                           : CHTHOLLY_NEXT_RESOURCE_OPERATION_COMMITTED;
  return chtholly_next_resource_operation_v2_complete(operation, terminal);
}

extern "C" ABI2_EXPORT int32_t chtholly_component_close_v2(void) {
  if (mode() == "close-fail-once" && close_attempts.fetch_add(1) == 0) return -1;
  std::vector<std::thread> joining;
  {
    std::lock_guard lock(workers_mutex);
    for (auto *operation : pending_operations)
      (void)chtholly_next_resource_operation_v2_request_cancel(operation);
    pending_operations.clear(); joining.swap(workers);
  }
  for (auto &worker : joining) worker.join();
  return 0;
}
