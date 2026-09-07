#include "chtholly/Compiler/LowIR.h"
#include "chtholly/Compiler/SemIR.h"
#include "LLVMInternal.h"


#include "test_check.h"
#include <cstdint>
#include <optional>

namespace chtholly::compiler {

} // namespace chtholly::compiler

int main() {
  using namespace chtholly::compiler;

  SemNominalField computed;
  computed.projection_kind = PublicObjectProjectionKind::Computed;
  CHTHOLLY_TEST_CHECK(!debugFieldInfo(computed, LowNominalFieldLayout{7, 4, 4}).has_value());

  SemNominalField bitpacked;
  bitpacked.projection_kind = PublicObjectProjectionKind::BitPacked;
  bitpacked.bit_begin = 3;
  bitpacked.bit_end = 9;
  const auto bit_info = debugFieldInfo(bitpacked, LowNominalFieldLayout{2, 2, 2});
  CHTHOLLY_TEST_CHECK(bit_info.has_value());
  CHTHOLLY_TEST_CHECK(bit_info->bit_field);
  CHTHOLLY_TEST_CHECK(bit_info->size_bits == 6);
  CHTHOLLY_TEST_CHECK(bit_info->offset_bits == 3);
  CHTHOLLY_TEST_CHECK(bit_info->storage_offset_bits == 16);

  SemNominalField stable;
  stable.projection_kind = PublicObjectProjectionKind::StableAddress;
  const auto stable_info = debugFieldInfo(stable, LowNominalFieldLayout{4, 8, 8});
  CHTHOLLY_TEST_CHECK(stable_info.has_value());
  CHTHOLLY_TEST_CHECK(!stable_info->bit_field);
  CHTHOLLY_TEST_CHECK(stable_info->size_bits == 64);
  CHTHOLLY_TEST_CHECK(stable_info->offset_bits == 32);
  CHTHOLLY_TEST_CHECK(stable_info->storage_offset_bits == 0);

  CHTHOLLY_TEST_CHECK(!debugFieldInfo(stable, LowNominalFieldLayout{4, 0, 1}).has_value());
  return 0;
}
