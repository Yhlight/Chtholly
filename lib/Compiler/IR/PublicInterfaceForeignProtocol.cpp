#include "ArtifactDecodeInternal.h"
#include "PublicInterfaceEncodingInternal.h"
#include "PublicInterfaceServices.h"
#include "chtholly/Compiler/PublicInterface.h"

#include <algorithm>
#include <limits>
#include <ranges>
#include <string>
#include <unordered_set>
#include <utility>

namespace chtholly::compiler {

using internal::appendField;
using internal::appendForeignSignature;
using internal::appendType;
using internal::appendU32;
using internal::appendU64;

StableFingerprint
foreignAbiSignatureFingerprint(const ForeignAbiSignature &signature) {
  std::string canonical = "chtholly.next.foreign-abi-signature.v2";
  appendForeignSignature(canonical, signature);
  return StableFingerprint::fromCanonicalBytes(canonical);
}

std::string_view foreignResourceRoleKindName(ForeignResourceRoleKind kind) {
  switch (kind) {
  case ForeignResourceRoleKind::AcquireOwned:
    return "acquire-owned";
  case ForeignResourceRoleKind::Borrow:
    return "borrow";
  case ForeignResourceRoleKind::CloseQuiescent:
    return "close-quiescent";
  case ForeignResourceRoleKind::CancelQuiescent:
    return "cancel-quiescent";
  case ForeignResourceRoleKind::CancelAsync:
    return "cancel-async";
  case ForeignResourceRoleKind::WaitCompletion:
    return "wait-completion";
  case ForeignResourceRoleKind::InspectReady:
    return "inspect-ready";
  case ForeignResourceRoleKind::ArmOneShot:
    return "arm-one-shot";
  case ForeignResourceRoleKind::DetachCompletion:
    return "detach-completion";
  case ForeignResourceRoleKind::Count:
    return "invalid";
  }
  return "invalid";
}

const ForeignResourceRole *
ForeignResourceProtocol::findRole(ForeignResourceRoleKind kind) const {
  const auto found =
      std::ranges::lower_bound(roles, kind, {}, &ForeignResourceRole::kind);
  return found != roles.end() && found->kind == kind ? &*found : nullptr;
}

bool ForeignResourceProtocol::verify(std::uint32_t type_count,
                                     std::string &error) const {
  error.clear();
  const auto valid_type = [&](std::uint32_t index) {
    return index == core::AnyId::InvalidIndex || index < type_count;
  };
  if (semantic_epoch != CurrentSemanticEpoch) {
    error = "foreign resource protocol has an unsupported semantic epoch";
    return false;
  }
  if (!valid_type(callback_type_index) || resource_type_index >= type_count ||
      !valid_type(completion_type_index) || release_authority > 1 ||
      invalid_state >= ForeignResourceInvalidState::Count || roles.empty()) {
    error = "foreign resource protocol has invalid core facts";
    return false;
  }
  ForeignResourceRoleKind previous = ForeignResourceRoleKind::Count;
  bool first = true;
  for (const auto &role : roles) {
    if (role.kind >= ForeignResourceRoleKind::Count ||
        role.callable_type_index >= type_count ||
        (!first && role.kind <= previous)) {
      error = "foreign resource protocol roles are not canonical and unique";
      return false;
    }
    first = false;
    previous = role.kind;
    const auto expected_quiescence = [&] {
      switch (role.kind) {
      case ForeignResourceRoleKind::CloseQuiescent:
      case ForeignResourceRoleKind::CancelQuiescent:
      case ForeignResourceRoleKind::WaitCompletion:
        return ForeignResourceQuiescence::Quiescent;
      case ForeignResourceRoleKind::CancelAsync:
      case ForeignResourceRoleKind::DetachCompletion:
        return ForeignResourceQuiescence::NonQuiescent;
      default:
        return ForeignResourceQuiescence::None;
      }
    }();
    if (role.quiescence != expected_quiescence) {
      error = "foreign resource role has inconsistent quiescence";
      return false;
    }
    std::vector<bool> occupied;
    ForeignResourceParameterBinding previous_parameter;
    bool first_parameter = true;
    for (const auto &parameter : role.parameters) {
      if (parameter.kind >= ForeignResourceParameterKind::Count ||
          parameter.parameter_index == core::AnyId::InvalidIndex ||
          (parameter.kind == ForeignResourceParameterKind::Bound) !=
              !parameter.name.empty() ||
          (!first_parameter && !(previous_parameter < parameter))) {
        error = "foreign resource role has invalid parameter bindings";
        return false;
      }
      first_parameter = false;
      previous_parameter = parameter;
      if (parameter.parameter_index >= occupied.size())
        occupied.resize(parameter.parameter_index + 1);
      if (occupied[parameter.parameter_index]) {
        error = "foreign resource role maps two facts to one parameter";
        return false;
      }
      occupied[parameter.parameter_index] = true;
    }
  }
  const auto has = [&](ForeignResourceRoleKind kind) {
    return findRole(kind) != nullptr;
  };
  const auto completion_roles = has(ForeignResourceRoleKind::CancelAsync) ||
                                has(ForeignResourceRoleKind::WaitCompletion) ||
                                has(ForeignResourceRoleKind::InspectReady) ||
                                has(ForeignResourceRoleKind::ArmOneShot) ||
                                has(ForeignResourceRoleKind::DetachCompletion);
  if (completion_roles && completion_type_index == core::AnyId::InvalidIndex) {
    error = "foreign resource completion roles require a completion type";
    return false;
  }
  if ((!completion_projection &&
       has(ForeignResourceRoleKind::CancelAsync) !=
           has(ForeignResourceRoleKind::WaitCompletion)) ||
      (has(ForeignResourceRoleKind::InspectReady) &&
       !has(ForeignResourceRoleKind::WaitCompletion)) ||
      has(ForeignResourceRoleKind::ArmOneShot) !=
          has(ForeignResourceRoleKind::DetachCompletion) ||
      (has(ForeignResourceRoleKind::ArmOneShot) &&
       !has(ForeignResourceRoleKind::InspectReady))) {
    error = "foreign resource completion role closure is incomplete";
    return false;
  }
  if (completion_projection && (has(ForeignResourceRoleKind::AcquireOwned) ||
                                has(ForeignResourceRoleKind::Borrow) ||
                                has(ForeignResourceRoleKind::CloseQuiescent) ||
                                has(ForeignResourceRoleKind::CancelQuiescent) ||
                                has(ForeignResourceRoleKind::CancelAsync))) {
    error = "foreign resource completion projection contains owner roles";
    return false;
  }
  const auto ordinary_cleanup =
      cleanup_path.size() == 1 &&
      (cleanup_path.front() == ForeignResourceRoleKind::CloseQuiescent ||
       cleanup_path.front() == ForeignResourceRoleKind::CancelQuiescent ||
       (completion_projection &&
        cleanup_path.front() == ForeignResourceRoleKind::WaitCompletion));
  const auto asynchronous_cleanup =
      !completion_projection && cleanup_path.size() == 2 &&
      cleanup_path[0] == ForeignResourceRoleKind::CancelAsync &&
      cleanup_path[1] == ForeignResourceRoleKind::WaitCompletion;
  if (!ordinary_cleanup && !asynchronous_cleanup) {
    error = "foreign resource protocol has an invalid cleanup path";
    return false;
  }
  for (const auto role : cleanup_path)
    if (!has(role)) {
      error = "foreign resource cleanup path role is absent";
      return false;
    }
  const auto valid_projection_cleanup = [&](const auto &path) {
    return path.size() == 1 &&
           path.front() == ForeignResourceRoleKind::WaitCompletion &&
           has(ForeignResourceRoleKind::WaitCompletion);
  };
  if (completion_type_index != core::AnyId::InvalidIndex &&
      !valid_projection_cleanup(completion_cleanup_path)) {
    error = "foreign resource completion cleanup path is invalid";
    return false;
  }
  if (has(ForeignResourceRoleKind::ArmOneShot) &&
      !valid_projection_cleanup(wake_cleanup_path)) {
    error = "foreign resource wake cleanup path is invalid";
    return false;
  }
  return true;
}

ForeignResourceProtocol
makeCallbackCompletionProtocol(std::uint8_t authority,
                               std::uint32_t argument_count,
                               std::array<std::uint32_t, 4> arm_parameters,
                               std::array<std::uint32_t, 3> detach_parameters) {
  ForeignResourceProtocol protocol;
  protocol.completion_projection = true;
  protocol.callback_type_index = 0;
  protocol.resource_type_index = 1;
  protocol.completion_type_index = 2;
  protocol.release_authority = authority;
  protocol.cleanup_path = {ForeignResourceRoleKind::WaitCompletion};
  protocol.completion_cleanup_path = {ForeignResourceRoleKind::WaitCompletion};
  protocol.roles.push_back(
      {.kind = ForeignResourceRoleKind::WaitCompletion,
       .callable_type_index = 3,
       .quiescence = ForeignResourceQuiescence::Quiescent,
       .parameters = {{ForeignResourceParameterKind::Completion, 0, {}}}});
  if (argument_count >= 5)
    protocol.roles.push_back(
        {.kind = ForeignResourceRoleKind::InspectReady,
         .callable_type_index = 4,
         .parameters = {{ForeignResourceParameterKind::Completion, 0, {}}}});
  if (argument_count == 7) {
    protocol.wake_cleanup_path = {ForeignResourceRoleKind::WaitCompletion};
    protocol.roles.push_back(
        {.kind = ForeignResourceRoleKind::ArmOneShot,
         .callable_type_index = 5,
         .parameters = {
             {ForeignResourceParameterKind::Completion, arm_parameters[0], {}},
             {ForeignResourceParameterKind::WakerEntry, arm_parameters[1], {}},
             {ForeignResourceParameterKind::WakerUserdata,
              arm_parameters[2],
              {}},
             {ForeignResourceParameterKind::WakerRelease,
              arm_parameters[3],
              {}}}});
    ForeignResourceRole detach{
        .kind = ForeignResourceRoleKind::DetachCompletion,
        .callable_type_index = 6,
        .quiescence = ForeignResourceQuiescence::NonQuiescent,
        .parameters = {{ForeignResourceParameterKind::Completion,
                        detach_parameters[0],
                        {}}}};
    if (authority == 0) {
      detach.parameters.push_back({ForeignResourceParameterKind::WakerUserdata,
                                   detach_parameters[1],
                                   {}});
      detach.parameters.push_back({ForeignResourceParameterKind::WakerRelease,
                                   detach_parameters[2],
                                   {}});
    }
    protocol.roles.push_back(std::move(detach));
  }
  internal::PublicInterfaceCanonicalizeService::foreignProtocol(protocol);
  return protocol;
}

ForeignResourceProtocol makeCallbackRegistrationProtocol(
    std::uint8_t authority, std::uint32_t entry_parameter,
    std::uint32_t userdata_parameter, std::uint32_t release_parameter,
    std::span<const CallbackRegistrationBinding> bindings,
    std::uint32_t argument_count, std::array<std::uint32_t, 4> arm_parameters,
    std::array<std::uint32_t, 3> detach_parameters) {
  ForeignResourceProtocol protocol;
  protocol.callback_type_index = 0;
  protocol.resource_type_index = 1;
  protocol.completion_type_index =
      argument_count >= 7 ? 1U : core::AnyId::InvalidIndex;
  protocol.release_authority = authority;
  protocol.cleanup_path = {ForeignResourceRoleKind::CloseQuiescent};
  ForeignResourceRole acquire{.kind = ForeignResourceRoleKind::AcquireOwned,
                              .callable_type_index = 2};
  acquire.parameters.push_back(
      {ForeignResourceParameterKind::CallbackEntry, entry_parameter, {}});
  acquire.parameters.push_back(
      {ForeignResourceParameterKind::CallbackUserdata, userdata_parameter, {}});
  if (authority == 1)
    acquire.parameters.push_back(
        {ForeignResourceParameterKind::CallbackRelease, release_parameter, {}});
  for (const auto &binding : bindings)
    acquire.parameters.push_back({ForeignResourceParameterKind::Bound,
                                  binding.parameter_index, binding.name});
  protocol.roles.push_back(std::move(acquire));
  protocol.roles.push_back(
      {.kind = ForeignResourceRoleKind::CloseQuiescent,
       .callable_type_index = 3,
       .quiescence = ForeignResourceQuiescence::Quiescent,
       .parameters = {{ForeignResourceParameterKind::Resource, 0, {}}}});
  protocol.roles.push_back(
      {.kind = ForeignResourceRoleKind::CancelQuiescent,
       .callable_type_index = 4,
       .quiescence = ForeignResourceQuiescence::Quiescent,
       .parameters = {{ForeignResourceParameterKind::Resource, 0, {}}}});
  if (argument_count >= 7) {
    protocol.completion_cleanup_path = {
        ForeignResourceRoleKind::WaitCompletion};
    protocol.roles.push_back(
        {.kind = ForeignResourceRoleKind::CancelAsync,
         .callable_type_index = 5,
         .quiescence = ForeignResourceQuiescence::NonQuiescent,
         .parameters = {{ForeignResourceParameterKind::Resource, 0, {}}}});
    protocol.roles.push_back(
        {.kind = ForeignResourceRoleKind::WaitCompletion,
         .callable_type_index = 6,
         .quiescence = ForeignResourceQuiescence::Quiescent,
         .parameters = {{ForeignResourceParameterKind::Completion, 0, {}}}});
  }
  if (argument_count >= 8)
    protocol.roles.push_back(
        {.kind = ForeignResourceRoleKind::InspectReady,
         .callable_type_index = 7,
         .parameters = {{ForeignResourceParameterKind::Completion, 0, {}}}});
  if (argument_count == 10) {
    protocol.wake_cleanup_path = {ForeignResourceRoleKind::WaitCompletion};
    protocol.roles.push_back(
        {.kind = ForeignResourceRoleKind::ArmOneShot,
         .callable_type_index = 8,
         .parameters = {
             {ForeignResourceParameterKind::Completion, arm_parameters[0], {}},
             {ForeignResourceParameterKind::WakerEntry, arm_parameters[1], {}},
             {ForeignResourceParameterKind::WakerUserdata,
              arm_parameters[2],
              {}},
             {ForeignResourceParameterKind::WakerRelease,
              arm_parameters[3],
              {}}}});
    ForeignResourceRole detach{
        .kind = ForeignResourceRoleKind::DetachCompletion,
        .callable_type_index = 9,
        .quiescence = ForeignResourceQuiescence::NonQuiescent,
        .parameters = {{ForeignResourceParameterKind::Completion,
                        detach_parameters[0],
                        {}}}};
    if (authority == 0) {
      detach.parameters.push_back({ForeignResourceParameterKind::WakerUserdata,
                                   detach_parameters[1],
                                   {}});
      detach.parameters.push_back({ForeignResourceParameterKind::WakerRelease,
                                   detach_parameters[2],
                                   {}});
    }
    protocol.roles.push_back(std::move(detach));
  }
  internal::PublicInterfaceCanonicalizeService::foreignProtocol(protocol);
  return protocol;
}

std::string
encodeForeignResourceProtocol(const ForeignResourceProtocol &protocol) {
  std::string out;
  appendU32(out, protocol.semantic_epoch);
  appendU32(out, protocol.completion_projection ? 1U : 0U);
  appendU32(out, protocol.callback_type_index);
  appendU32(out, protocol.resource_type_index);
  appendU32(out, protocol.completion_type_index);
  appendU32(out, static_cast<std::uint32_t>(protocol.invalid_state));
  appendU64(out, static_cast<std::uint64_t>(protocol.invalid_integer));
  appendU32(out, protocol.release_authority);
  appendU32(out, static_cast<std::uint32_t>(protocol.cleanup_path.size()));
  for (const auto role : protocol.cleanup_path)
    appendU32(out, static_cast<std::uint32_t>(role));
  appendU32(
      out, static_cast<std::uint32_t>(protocol.completion_cleanup_path.size()));
  for (const auto role : protocol.completion_cleanup_path)
    appendU32(out, static_cast<std::uint32_t>(role));
  appendU32(out, static_cast<std::uint32_t>(protocol.wake_cleanup_path.size()));
  for (const auto role : protocol.wake_cleanup_path)
    appendU32(out, static_cast<std::uint32_t>(role));
  appendU32(out, static_cast<std::uint32_t>(protocol.roles.size()));
  for (const auto &role : protocol.roles) {
    appendU32(out, static_cast<std::uint32_t>(role.kind));
    appendU32(out, role.callable_type_index);
    appendU32(out, static_cast<std::uint32_t>(role.quiescence));
    appendU32(out, static_cast<std::uint32_t>(role.parameters.size()));
    for (const auto &parameter : role.parameters) {
      appendU32(out, static_cast<std::uint32_t>(parameter.kind));
      appendU32(out, parameter.parameter_index);
      appendField(out, parameter.name);
    }
  }
  return out;
}

std::optional<ForeignResourceProtocol>
decodeForeignResourceProtocol(std::string_view bytes, std::uint32_t type_count,
                              std::string &error) {
  error.clear();
  internal::ArtifactDecodeContext context(bytes.size());
  if (context.failed()) {
    context.preferBudgetError(error);
    return std::nullopt;
  }
  return internal::decodeForeignResourceProtocol(bytes, type_count, error,
                                                 context);
}

std::optional<ForeignResourceProtocol> internal::decodeForeignResourceProtocol(
    std::string_view bytes, std::uint32_t type_count, std::string &error,
    internal::ArtifactDecodeContext &context) {
  internal::ArtifactDecodeErrorScope error_scope(context, error);
  std::size_t offset = 0;
  const auto read_u32 = [&](std::uint32_t &value) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
      return false;
    value = 0;
    for (std::uint32_t index = 0; index < 4; ++index)
      value |= static_cast<std::uint32_t>(
                   static_cast<unsigned char>(bytes[offset + index]))
               << (index * 8U);
    offset += 4;
    return true;
  };
  const auto read_string = [&](std::string &value) {
    std::uint32_t size = 0;
    if (!read_u32(size) || offset > bytes.size() ||
        size > bytes.size() - offset || !context.consumeString(size))
      return false;
    value.assign(bytes.substr(offset, size));
    offset += size;
    return true;
  };
  ForeignResourceProtocol protocol;
  std::uint32_t projection = 0, invalid_state = 0, authority = 0,
                cleanup_count = 0, role_count = 0;
  std::uint64_t invalid_integer_bits = 0;
  const auto read_u64 = [&](std::uint64_t &value) {
    if (offset > bytes.size() || bytes.size() - offset < 8)
      return false;
    value = 0;
    for (std::uint32_t index = 0; index < 8; ++index)
      value |= static_cast<std::uint64_t>(
                   static_cast<unsigned char>(bytes[offset + index]))
               << (index * 8U);
    offset += 8;
    return true;
  };
  if (!read_u32(protocol.semantic_epoch) || !read_u32(projection) ||
      projection > 1 || !read_u32(protocol.callback_type_index) ||
      !read_u32(protocol.resource_type_index) ||
      !read_u32(protocol.completion_type_index) || !read_u32(invalid_state) ||
      !read_u64(invalid_integer_bits) || !read_u32(authority) ||
      !read_u32(cleanup_count) || cleanup_count > 8 ||
      !context.consumeNodes(cleanup_count)) {
    error = "foreign resource protocol encoding is truncated";
    return std::nullopt;
  }
  protocol.completion_projection = projection != 0;
  protocol.invalid_state =
      static_cast<ForeignResourceInvalidState>(invalid_state);
  protocol.invalid_integer = static_cast<std::int64_t>(invalid_integer_bits);
  protocol.release_authority = static_cast<std::uint8_t>(authority);
  protocol.cleanup_path.resize(cleanup_count);
  for (auto &cleanup : protocol.cleanup_path) {
    std::uint32_t role = 0;
    if (!read_u32(role)) {
      error = "foreign resource cleanup path encoding is truncated";
      return std::nullopt;
    }
    cleanup = static_cast<ForeignResourceRoleKind>(role);
  }
  const auto read_cleanup_path = [&](auto &path) {
    std::uint32_t count = 0;
    if (!read_u32(count) || count > 8 || !context.consumeNodes(count))
      return false;
    path.resize(count);
    for (auto &item : path) {
      std::uint32_t role = 0;
      if (!read_u32(role))
        return false;
      item = static_cast<ForeignResourceRoleKind>(role);
    }
    return true;
  };
  if (!read_cleanup_path(protocol.completion_cleanup_path) ||
      !read_cleanup_path(protocol.wake_cleanup_path)) {
    error = "foreign resource projected cleanup path encoding is truncated";
    return std::nullopt;
  }
  if (!read_u32(role_count) || role_count > 64 ||
      !context.consumeNodes(role_count)) {
    error = "foreign resource protocol encoding is truncated";
    return std::nullopt;
  }
  protocol.roles.resize(role_count);
  for (auto &role : protocol.roles) {
    std::uint32_t kind = 0, quiescence = 0, parameter_count = 0;
    if (!read_u32(kind) || !read_u32(role.callable_type_index) ||
        !read_u32(quiescence) || !read_u32(parameter_count) ||
        parameter_count > 64 || !context.consumeNodes(parameter_count)) {
      error = "foreign resource role encoding is truncated";
      return std::nullopt;
    }
    role.kind = static_cast<ForeignResourceRoleKind>(kind);
    role.quiescence = static_cast<ForeignResourceQuiescence>(quiescence);
    role.parameters.resize(parameter_count);
    for (auto &parameter : role.parameters) {
      std::uint32_t parameter_kind = 0;
      if (!read_u32(parameter_kind) || !read_u32(parameter.parameter_index) ||
          !read_string(parameter.name)) {
        error = "foreign resource parameter encoding is truncated";
        return std::nullopt;
      }
      parameter.kind =
          static_cast<ForeignResourceParameterKind>(parameter_kind);
    }
  }
  if (offset != bytes.size() || !protocol.verify(type_count, error)) {
    if (error.empty())
      error = "foreign resource protocol encoding has trailing bytes";
    return std::nullopt;
  }
  context.preferBudgetError(error);
  return protocol;
}

} // namespace chtholly::compiler
