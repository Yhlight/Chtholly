#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace chtholly::compiler {

enum class CompilerIntrinsicRole : std::uint8_t {
  None,
  AtomicInit,
  AtomicLoad,
  AtomicStore,
  AtomicExchange,
  AtomicCompareExchange,
  AtomicFetchAdd,
  AtomicFetchSub,
  AtomicFetchAnd,
  AtomicFetchOr,
  AtomicFetchXor,
  VolatileLoad,
  VolatileStore,
  EnvArgCount,
  EnvArg,
  IoWriteStdout,
  IoWriteStderr,
  VecInit,
  VecLen,
  VecCapacity,
  VecReserve,
  VecPush,
  VecAt,
  VecAtMut,
  VecPop,
  VecRemove,
  VecClear,
  VecDrop,
  VecIter,
  VecIterMut,
  VecIterNext,
  VecIterMutNext,
  OptionIsSome,
  OptionIsNone,
  OptionUnwrap,
  OptionAsRef,
  OptionAsMut,
  FsExists,
  FsWrite,
  FsRemove,
  WrappingMul,
  FloatHash,
  FloatEqual,
  PointerHash,
  PointerEqual,
  HashMapMake,
  HashMapLen,
  HashMapCapacity,
  HashMapIsEmpty,
  HashMapContains,
  HashMapGet,
  HashMapGetMut,
  HashMapInsert,
  HashMapRemove,
  HashMapReserve,
  HashMapClear,
  HashMapDrop,
  HashSetMake,
  HashSetLen,
  HashSetCapacity,
  HashSetIsEmpty,
  HashSetContains,
  HashSetInsert,
  HashSetRemove,
  HashSetReserve,
  HashSetClear,
  HashSetDrop,
  TextAsBytes,
  TextSliceData,
  TextSliceDataMut,
  // Typed-channel operations are compiler-owned. Keep these after the
  // historical roles so serialized intrinsic identities remain stable.
  ChannelMake,
  ChannelSendPrepare,
  ChannelSendCommit,
  ChannelSendCancel,
  ChannelReceiveAcquire,
  ChannelReceiveCommit,
  ChannelReceiveCancel,
  ChannelClose,
  // Stable source-level facade. The prepare/commit roles above remain
  // compiler-internal transition hooks; these roles return Result<void,
  // ErrorCode> and hide token management from source code.
  ChannelInit,
  ChannelSend,
  ChannelReceive,
  ChannelDrop,
  Count,
};

[[nodiscard]] constexpr std::string_view
compilerIntrinsicRoleName(CompilerIntrinsicRole role) {
  switch (role) {
  case CompilerIntrinsicRole::None:
    return "none";
  case CompilerIntrinsicRole::AtomicInit:
    return "atomic.init";
  case CompilerIntrinsicRole::AtomicLoad:
    return "atomic.load";
  case CompilerIntrinsicRole::AtomicStore:
    return "atomic.store";
  case CompilerIntrinsicRole::AtomicExchange:
    return "atomic.exchange";
  case CompilerIntrinsicRole::AtomicCompareExchange:
    return "atomic.compare-exchange";
  case CompilerIntrinsicRole::AtomicFetchAdd:
    return "atomic.fetch-add";
  case CompilerIntrinsicRole::AtomicFetchSub:
    return "atomic.fetch-sub";
  case CompilerIntrinsicRole::AtomicFetchAnd:
    return "atomic.fetch-and";
  case CompilerIntrinsicRole::AtomicFetchOr:
    return "atomic.fetch-or";
  case CompilerIntrinsicRole::AtomicFetchXor:
    return "atomic.fetch-xor";
  case CompilerIntrinsicRole::VolatileLoad:
    return "volatile.load";
  case CompilerIntrinsicRole::VolatileStore:
    return "volatile.store";
  case CompilerIntrinsicRole::EnvArgCount:
    return "env.arg-count";
  case CompilerIntrinsicRole::EnvArg:
    return "env.arg";
  case CompilerIntrinsicRole::IoWriteStdout:
    return "io.write-stdout";
  case CompilerIntrinsicRole::IoWriteStderr:
    return "io.write-stderr";
  case CompilerIntrinsicRole::VecInit:
    return "vec.init";
  case CompilerIntrinsicRole::VecLen:
    return "vec.len";
  case CompilerIntrinsicRole::VecCapacity:
    return "vec.capacity";
  case CompilerIntrinsicRole::VecReserve:
    return "vec.reserve";
  case CompilerIntrinsicRole::VecPush:
    return "vec.push";
  case CompilerIntrinsicRole::VecAt:
    return "vec.at";
  case CompilerIntrinsicRole::VecAtMut:
    return "vec.at-mut";
  case CompilerIntrinsicRole::VecPop:
    return "vec.pop";
  case CompilerIntrinsicRole::VecRemove:
    return "vec.remove";
  case CompilerIntrinsicRole::VecClear:
    return "vec.clear";
  case CompilerIntrinsicRole::VecDrop:
    return "vec.drop";
  case CompilerIntrinsicRole::VecIter:
    return "vec.iter";
  case CompilerIntrinsicRole::VecIterMut:
    return "vec.iter-mut";
  case CompilerIntrinsicRole::VecIterNext:
    return "vec.iter-next";
  case CompilerIntrinsicRole::VecIterMutNext:
    return "vec.iter-mut-next";
  case CompilerIntrinsicRole::OptionIsSome:
    return "option.is-some";
  case CompilerIntrinsicRole::OptionIsNone:
    return "option.is-none";
  case CompilerIntrinsicRole::OptionUnwrap:
    return "option.unwrap";
  case CompilerIntrinsicRole::OptionAsRef:
    return "option.as-ref";
  case CompilerIntrinsicRole::OptionAsMut:
    return "option.as-mut";
  case CompilerIntrinsicRole::FsExists:
    return "fs.exists";
  case CompilerIntrinsicRole::FsWrite:
    return "fs.write";
  case CompilerIntrinsicRole::FsRemove:
    return "fs.remove";
  case CompilerIntrinsicRole::WrappingMul:
    return "hash.wrapping-mul";
  case CompilerIntrinsicRole::FloatHash:
    return "hash.float";
  case CompilerIntrinsicRole::FloatEqual:
    return "equal.float";
  case CompilerIntrinsicRole::PointerHash:
    return "hash.pointer";
  case CompilerIntrinsicRole::PointerEqual:
    return "equal.pointer";
  case CompilerIntrinsicRole::HashMapMake:
    return "container.hashmap-make";
  case CompilerIntrinsicRole::HashMapLen:
    return "container.hashmap-len";
  case CompilerIntrinsicRole::HashMapCapacity:
    return "container.hashmap-capacity";
  case CompilerIntrinsicRole::HashMapIsEmpty:
    return "container.hashmap-is-empty";
  case CompilerIntrinsicRole::HashMapContains:
    return "container.hashmap-contains";
  case CompilerIntrinsicRole::HashMapGet:
    return "container.hashmap-get";
  case CompilerIntrinsicRole::HashMapGetMut:
    return "container.hashmap-get-mut";
  case CompilerIntrinsicRole::HashMapInsert:
    return "container.hashmap-insert";
  case CompilerIntrinsicRole::HashMapRemove:
    return "container.hashmap-remove";
  case CompilerIntrinsicRole::HashMapReserve:
    return "container.hashmap-reserve";
  case CompilerIntrinsicRole::HashMapClear:
    return "container.hashmap-clear";
  case CompilerIntrinsicRole::HashMapDrop:
    return "container.hashmap-drop";
  case CompilerIntrinsicRole::HashSetMake:
    return "container.hashset-make";
  case CompilerIntrinsicRole::HashSetLen:
    return "container.hashset-len";
  case CompilerIntrinsicRole::HashSetCapacity:
    return "container.hashset-capacity";
  case CompilerIntrinsicRole::HashSetIsEmpty:
    return "container.hashset-is-empty";
  case CompilerIntrinsicRole::HashSetContains:
    return "container.hashset-contains";
  case CompilerIntrinsicRole::HashSetInsert:
    return "container.hashset-insert";
  case CompilerIntrinsicRole::HashSetRemove:
    return "container.hashset-remove";
  case CompilerIntrinsicRole::HashSetReserve:
    return "container.hashset-reserve";
  case CompilerIntrinsicRole::HashSetClear:
    return "container.hashset-clear";
  case CompilerIntrinsicRole::HashSetDrop:
    return "container.hashset-drop";
  case CompilerIntrinsicRole::TextAsBytes:
    return "text.as-bytes";
  case CompilerIntrinsicRole::TextSliceData:
    return "text.slice-data";
  case CompilerIntrinsicRole::TextSliceDataMut:
    return "text.slice-data-mut";
  case CompilerIntrinsicRole::ChannelMake:
    return "channel.make";
  case CompilerIntrinsicRole::ChannelSendPrepare:
    return "channel.send-prepare";
  case CompilerIntrinsicRole::ChannelSendCommit:
    return "channel.send-commit";
  case CompilerIntrinsicRole::ChannelSendCancel:
    return "channel.send-cancel";
  case CompilerIntrinsicRole::ChannelReceiveAcquire:
    return "channel.receive-acquire";
  case CompilerIntrinsicRole::ChannelReceiveCommit:
    return "channel.receive-commit";
  case CompilerIntrinsicRole::ChannelReceiveCancel:
    return "channel.receive-cancel";
  case CompilerIntrinsicRole::ChannelClose:
    return "channel.close";
  case CompilerIntrinsicRole::ChannelInit:
    return "channel.init";
  case CompilerIntrinsicRole::ChannelSend:
    return "channel.send";
  case CompilerIntrinsicRole::ChannelReceive:
    return "channel.receive";
  case CompilerIntrinsicRole::ChannelDrop:
    return "channel.drop";
  case CompilerIntrinsicRole::Count:
    return "invalid";
  }
  return "invalid";
}

[[nodiscard]] constexpr std::optional<CompilerIntrinsicRole>
parseCompilerIntrinsicRole(std::string_view name) {
  for (std::uint8_t value = 1;
       value < static_cast<std::uint8_t>(CompilerIntrinsicRole::Count);
       ++value) {
    const auto role = static_cast<CompilerIntrinsicRole>(value);
    if (compilerIntrinsicRoleName(role) == name)
      return role;
  }
  return std::nullopt;
}

[[nodiscard]] constexpr bool
isAtomicCompilerIntrinsic(CompilerIntrinsicRole role) {
  return role >= CompilerIntrinsicRole::AtomicInit &&
         role <= CompilerIntrinsicRole::AtomicFetchXor;
}

[[nodiscard]] constexpr bool
isAtomicFetchCompilerIntrinsic(CompilerIntrinsicRole role) {
  return role >= CompilerIntrinsicRole::AtomicFetchAdd &&
         role <= CompilerIntrinsicRole::AtomicFetchXor;
}

[[nodiscard]] constexpr bool
isVolatileCompilerIntrinsic(CompilerIntrinsicRole role) {
  return role == CompilerIntrinsicRole::VolatileLoad ||
         role == CompilerIntrinsicRole::VolatileStore;
}

[[nodiscard]] constexpr bool
isVecCompilerIntrinsic(CompilerIntrinsicRole role) {
  return role >= CompilerIntrinsicRole::VecInit &&
         role <= CompilerIntrinsicRole::VecIterMutNext;
}

[[nodiscard]] constexpr bool
isOptionCompilerIntrinsic(CompilerIntrinsicRole role) {
  return role >= CompilerIntrinsicRole::OptionIsSome &&
         role <= CompilerIntrinsicRole::OptionAsMut;
}

[[nodiscard]] constexpr bool
isHashMapCompilerIntrinsic(CompilerIntrinsicRole role) {
  return role >= CompilerIntrinsicRole::HashMapMake &&
         role <= CompilerIntrinsicRole::HashMapDrop;
}

[[nodiscard]] constexpr bool
isHashSetCompilerIntrinsic(CompilerIntrinsicRole role) {
  return role >= CompilerIntrinsicRole::HashSetMake &&
         role <= CompilerIntrinsicRole::HashSetDrop;
}

[[nodiscard]] constexpr bool
isContainerCompilerIntrinsic(CompilerIntrinsicRole role) {
  return isHashMapCompilerIntrinsic(role) || isHashSetCompilerIntrinsic(role);
}

[[nodiscard]] constexpr bool
isChannelCompilerIntrinsic(CompilerIntrinsicRole role) {
  return role >= CompilerIntrinsicRole::ChannelMake &&
         role <= CompilerIntrinsicRole::ChannelDrop;
}

[[nodiscard]] constexpr bool
isChannelTransitionCompilerIntrinsic(CompilerIntrinsicRole role) {
  return role >= CompilerIntrinsicRole::ChannelSendPrepare &&
         role <= CompilerIntrinsicRole::ChannelReceiveCancel;
}

[[nodiscard]] constexpr bool
compilerIntrinsicRequiresInteger(CompilerIntrinsicRole role) {
  return isAtomicFetchCompilerIntrinsic(role) ||
         isVolatileCompilerIntrinsic(role) ||
         role == CompilerIntrinsicRole::WrappingMul;
}

[[nodiscard]] constexpr std::uint8_t
compilerIntrinsicParameterCount(CompilerIntrinsicRole role) {
  switch (role) {
  case CompilerIntrinsicRole::AtomicInit:
  case CompilerIntrinsicRole::VolatileLoad:
  case CompilerIntrinsicRole::EnvArg:
  case CompilerIntrinsicRole::IoWriteStdout:
  case CompilerIntrinsicRole::IoWriteStderr:
  case CompilerIntrinsicRole::FsExists:
  case CompilerIntrinsicRole::FsRemove:
    return 1;
  case CompilerIntrinsicRole::VecInit:
    return 0;
  case CompilerIntrinsicRole::VecLen:
  case CompilerIntrinsicRole::VecCapacity:
  case CompilerIntrinsicRole::VecDrop:
  case CompilerIntrinsicRole::VecClear:
  case CompilerIntrinsicRole::VecIter:
  case CompilerIntrinsicRole::VecIterMut:
  case CompilerIntrinsicRole::VecIterNext:
  case CompilerIntrinsicRole::VecIterMutNext:
  case CompilerIntrinsicRole::OptionIsSome:
  case CompilerIntrinsicRole::OptionIsNone:
  case CompilerIntrinsicRole::OptionUnwrap:
  case CompilerIntrinsicRole::OptionAsRef:
  case CompilerIntrinsicRole::OptionAsMut:
    return 1;
  case CompilerIntrinsicRole::VecReserve:
  case CompilerIntrinsicRole::VecPush:
  case CompilerIntrinsicRole::VecAt:
  case CompilerIntrinsicRole::VecAtMut:
  case CompilerIntrinsicRole::VecRemove:
  case CompilerIntrinsicRole::FsWrite:
    return 2;
  case CompilerIntrinsicRole::WrappingMul:
  case CompilerIntrinsicRole::FloatHash:
  case CompilerIntrinsicRole::FloatEqual:
  case CompilerIntrinsicRole::PointerHash:
  case CompilerIntrinsicRole::PointerEqual:
    return 2;
  case CompilerIntrinsicRole::HashMapMake:
  case CompilerIntrinsicRole::HashSetMake:
    return 0;
  case CompilerIntrinsicRole::HashMapLen:
  case CompilerIntrinsicRole::HashMapCapacity:
  case CompilerIntrinsicRole::HashMapIsEmpty:
  case CompilerIntrinsicRole::HashMapDrop:
  case CompilerIntrinsicRole::HashSetLen:
  case CompilerIntrinsicRole::HashSetCapacity:
  case CompilerIntrinsicRole::HashSetIsEmpty:
  case CompilerIntrinsicRole::HashSetDrop:
  case CompilerIntrinsicRole::HashMapClear:
  case CompilerIntrinsicRole::HashSetClear:
    return 1;
  case CompilerIntrinsicRole::TextAsBytes:
    return 1;
  case CompilerIntrinsicRole::TextSliceData:
  case CompilerIntrinsicRole::TextSliceDataMut:
    return 1;
  case CompilerIntrinsicRole::ChannelMake:
  case CompilerIntrinsicRole::ChannelInit:
    return 2;
  case CompilerIntrinsicRole::ChannelSendPrepare:
  case CompilerIntrinsicRole::ChannelSendCommit:
  case CompilerIntrinsicRole::ChannelReceiveCommit:
    return 2;
  case CompilerIntrinsicRole::ChannelSendCancel:
  case CompilerIntrinsicRole::ChannelReceiveAcquire:
  case CompilerIntrinsicRole::ChannelReceiveCancel:
  case CompilerIntrinsicRole::ChannelClose:
    return 1;
  case CompilerIntrinsicRole::ChannelSend:
    return 2;
  case CompilerIntrinsicRole::ChannelReceive:
    return 1;
  case CompilerIntrinsicRole::ChannelDrop:
    return 1;
  case CompilerIntrinsicRole::HashMapContains:
  case CompilerIntrinsicRole::HashMapGet:
  case CompilerIntrinsicRole::HashMapGetMut:
  case CompilerIntrinsicRole::HashMapRemove:
  case CompilerIntrinsicRole::HashMapReserve:
  case CompilerIntrinsicRole::HashSetContains:
  case CompilerIntrinsicRole::HashSetRemove:
  case CompilerIntrinsicRole::HashSetReserve:
    return 2;
  case CompilerIntrinsicRole::HashMapInsert:
    return 3;
  case CompilerIntrinsicRole::HashSetInsert:
    return 2;
  case CompilerIntrinsicRole::VecPop:
    return 1;
  case CompilerIntrinsicRole::AtomicLoad:
  case CompilerIntrinsicRole::VolatileStore:
    return 2;
  case CompilerIntrinsicRole::AtomicStore:
  case CompilerIntrinsicRole::AtomicExchange:
  case CompilerIntrinsicRole::AtomicFetchAdd:
  case CompilerIntrinsicRole::AtomicFetchSub:
  case CompilerIntrinsicRole::AtomicFetchAnd:
  case CompilerIntrinsicRole::AtomicFetchOr:
  case CompilerIntrinsicRole::AtomicFetchXor:
    return 3;
  case CompilerIntrinsicRole::AtomicCompareExchange:
    return 5;
  case CompilerIntrinsicRole::None:
  case CompilerIntrinsicRole::EnvArgCount:
  case CompilerIntrinsicRole::Count:
    return 0;
  }
  return 0;
}

[[nodiscard]] constexpr std::uint8_t
compilerIntrinsicOrderingCount(CompilerIntrinsicRole role) {
  return role == CompilerIntrinsicRole::AtomicCompareExchange
             ? 2
         : role >= CompilerIntrinsicRole::AtomicLoad &&
                 role <= CompilerIntrinsicRole::AtomicFetchXor
             ? 1
             : 0;
}

[[nodiscard]] constexpr std::uint8_t
compilerIntrinsicOrderingParameter(CompilerIntrinsicRole role,
                                   std::uint8_t ordering) {
  if (role == CompilerIntrinsicRole::AtomicLoad)
    return 1;
  if (role == CompilerIntrinsicRole::AtomicCompareExchange)
    return static_cast<std::uint8_t>(3 + ordering);
  return 2;
}

[[nodiscard]] constexpr bool
isValidCompilerIntrinsicOrdering(CompilerIntrinsicRole role,
                                 std::uint8_t ordering,
                                 std::uint8_t value) {
  if (value > 4)
    return false;
  if (role == CompilerIntrinsicRole::AtomicLoad)
    return value == 0 || value == 1 || value == 4;
  if (role == CompilerIntrinsicRole::AtomicStore)
    return value == 0 || value == 2 || value == 4;
  if (role == CompilerIntrinsicRole::AtomicCompareExchange && ordering == 1)
    return value == 0 || value == 1 || value == 4;
  return role == CompilerIntrinsicRole::AtomicCompareExchange ||
         role == CompilerIntrinsicRole::AtomicExchange ||
         isAtomicFetchCompilerIntrinsic(role);
}

[[nodiscard]] constexpr bool
isValidCompareExchangeOrderingPair(std::uint8_t success,
                                   std::uint8_t failure) {
  return failure == 0 ||
         (failure == 1 &&
          (success == 1 || success == 3 || success == 4)) ||
         (failure == 4 && success == 4);
}

struct CompilerIntrinsicBinding {
  std::string module_name;
  std::string entity_name;
  CompilerIntrinsicRole role = CompilerIntrinsicRole::None;

  friend bool operator==(const CompilerIntrinsicBinding &,
                         const CompilerIntrinsicBinding &) = default;
};

} // namespace chtholly::compiler
