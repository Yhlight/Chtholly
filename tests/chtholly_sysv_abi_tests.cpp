#include "chtholly/Compiler/LLVM.h"
#include "chtholly/Compiler/LowerToLowIR.h"
#include "chtholly/Compiler/PublicInterface.h"
#include "chtholly/Compiler/SemIR.h"
#include "chtholly/Compiler/SharedValueStores.h"

#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

void checkAt(bool condition, const char *expression, int line) {
  if (!condition) {
    std::fprintf(stderr, "CHECK failed at line %d: %s\n", line, expression);
    std::abort();
  }
}
#define CHECK(condition) checkAt((condition), #condition, __LINE__)

using namespace chtholly::compiler;

struct Fixture {
  chtholly::core::Arena arena;
  SharedValueStores values;
  PublicInterfaceRegistry public_interfaces{values};
  interop::ArtifactRegistry interop_registry;
  SemIR sem_ir{arena,
               values,
               CheckIRId(0),
               values.internIdentifier("sysv_abi"),
               public_interfaces,
               interop_registry};

  Fixture() {
    sem_ir.setTopBlock(sem_ir.addInstBlock({}));
    sem_ir.setPlaceStateQuery(PlaceStateQuery{});
  }

  TypeId nominal(std::string_view name, NominalKind kind,
                 std::span<const TypeId> field_types) {
    SemNominalType nominal;
    nominal.name = sem_ir.addName(values.internIdentifier(name));
    nominal.declaration = NodeId(0);
    nominal.flags = SemNominalTypePublic;
    nominal.representation_policy = NominalRepresentationPolicy::C;
    nominal.kind = kind;
    nominal.completion_state = SemNominalCompletionState::Complete;
    for (std::uint32_t index = 0; index < field_types.size(); ++index) {
      SemNominalField field;
      field.name = sem_ir.addName(
          values.internIdentifier("field" + std::to_string(index)));
      field.type = field_types[index];
      field.declaration = NodeId(index);
      nominal.fields.push_back(std::move(field));
    }
    return sem_ir.addNominalType(sem_ir.addNominalTypeDecl(std::move(nominal)));
  }

  FunctionRefId foreignFunction(std::string_view name,
                                std::span<const TypeId> parameters,
                                TypeId result) {
    const auto function_type = sem_ir.addFunctionType(parameters, result);
    SemFunction function_value;
    function_value.name = sem_ir.addName(values.internIdentifier(name));
    function_value.type = function_type;
    function_value.parameters = sem_ir.addLocalBlock({});
    function_value.body = sem_ir.addInstBlock({});
    function_value.flags = SemFunctionPublic;
    const auto function = sem_ir.addFunction(std::move(function_value));
    const auto classify = [&](TypeId type, bool is_result) {
      const auto &semantic = sem_ir.type(type);
      ForeignAbiValue value;
      if (semantic.kind == SemTypeKind::Void && is_result)
        value.kind = ForeignAbiValueKind::Void;
      else if (semantic.kind == SemTypeKind::Integer) {
        value.kind = semantic.arg1 != 0 ? ForeignAbiValueKind::SignedInteger
                                        : ForeignAbiValueKind::UnsignedInteger;
        value.width = semantic.arg0;
      } else if (semantic.kind == SemTypeKind::Float) {
        value.kind = ForeignAbiValueKind::Float;
        value.width = semantic.arg0;
      } else if (semantic.kind == SemTypeKind::Nominal) {
        value.kind = ForeignAbiValueKind::Aggregate;
      }
      return value;
    };
    ForeignAbiSignature signature;
    signature.result = classify(result, true);
    for (const auto parameter : parameters)
      signature.parameters.push_back(classify(parameter, false));
    SemCallableDeclaration declaration;
    declaration.kind = SemCallableDeclarationKind::Foreign;
    declaration.is_unsafe = true;
    declaration.foreign_abi = values.internIdentifier("C");
    declaration.foreign_signature = std::move(signature);
    sem_ir.setFunctionDeclaration(function, std::move(declaration));
    SemFunctionRef reference;
    reference.local_function = function;
    reference.local_type = function_type;
    return sem_ir.addFunctionRef(std::move(reference));
  }
};

std::unordered_map<std::string, const ForeignAbiFunctionLayout *>
layoutsByName(const LowIR &low_ir) {
  std::unordered_map<std::string, const ForeignAbiFunctionLayout *> result;
  const auto &sem_ir = low_ir.semIR();
  for (std::uint32_t index = 0; index < sem_ir.functionRefCount(); ++index) {
    const auto target = FunctionRefId(index);
    const auto layout_id = low_ir.foreignAbiLayoutFor(target);
    if (!layout_id.hasValue())
      continue;
    const auto &reference = sem_ir.functionRef(target);
    CHECK(reference.local_function.hasValue());
    const auto &function = sem_ir.function(reference.local_function);
    result.emplace(
        std::string(sem_ir.identifier(sem_ir.name(function.name).text)),
        &low_ir.foreignAbiLayout(layout_id));
  }
  return result;
}

const ForeignAbiFunctionLayout &requireLayout(
    const std::unordered_map<std::string, const ForeignAbiFunctionLayout *>
        &values,
    std::string_view name) {
  const auto found = values.find(std::string(name));
  CHECK(found != values.end());
  return *found->second;
}

} // namespace

int main() {
  Fixture fixture;
  const auto i8 = fixture.sem_ir.addIntegerType(8, true);
  const auto u16 = fixture.sem_ir.addIntegerType(16, false);
  const auto i64 = fixture.sem_ir.addIntegerType(64, true);
  const auto f32 = fixture.sem_ir.addFloatType(32);
  const auto f64 = fixture.sem_ir.addFloatType(64);
  const auto i32_array = fixture.sem_ir.addType(
      {SemTypeKind::Array, fixture.sem_ir.i32Type().index, 4});
  const auto i64_box =
      fixture.nominal("I64Box", NominalKind::Struct, {&i64, 1});
  const auto f64_box =
      fixture.nominal("F64Box", NominalKind::Struct, {&f64, 1});
  const std::vector<TypeId> pair_fields = {f32, f32};
  const auto f32_pair =
      fixture.nominal("F32Pair", NominalKind::Struct, pair_fields);
  const std::vector<TypeId> mixed_fields = {i64, f64};
  const auto mixed =
      fixture.nominal("Mixed", NominalKind::Struct, mixed_fields);
  const std::vector<TypeId> large_fields = {i64, i64, i64};
  const auto large =
      fixture.nominal("Large", NominalKind::Struct, large_fields);
  const std::vector<TypeId> nested_fields = {f32_pair, i64};
  const auto nested =
      fixture.nominal("Nested", NominalKind::Struct, nested_fields);
  const auto array_box =
      fixture.nominal("ArrayBox", NominalKind::Struct, {&i32_array, 1});
  const std::vector<TypeId> union_fields = {i64, f64};
  const auto number =
      fixture.nominal("Number", NominalKind::Union, union_fields);

  const std::vector<TypeId> narrow_parameters = {i8, u16};
  fixture.foreignFunction("narrow", narrow_parameters,
                          fixture.sem_ir.i32Type());
  for (const auto &[name, type] :
       std::vector<std::pair<std::string_view, TypeId>>{
           {"i64_box", i64_box},
           {"f64_box", f64_box},
           {"f32_pair", f32_pair},
           {"mixed", mixed},
           {"large", large},
           {"nested", nested},
           {"array_box", array_box},
           {"number", number}})
    fixture.foreignFunction(name, {&type, 1}, type);

  auto low_ir =
      lowerToLowIR(fixture.sem_ir, fixture.arena, "x86_64-unknown-linux-gnu");
  std::string error;
  if (!low_ir.verify(error))
    std::fprintf(stderr, "SysV LowIR verification failed: %s\n", error.c_str());
  CHECK(error.empty());
  const auto layouts = layoutsByName(low_ir);

  const auto &narrow = requireLayout(layouts, "narrow");
  CHECK(narrow.parameters.size() == 2);
  CHECK(narrow.parameters[0].kind == ForeignPassKind::Scalar);
  CHECK(narrow.parameters[0].extension == ForeignExtensionKind::Sign);
  CHECK(narrow.parameters[1].extension == ForeignExtensionKind::Zero);

  const auto directLane = [&](std::string_view name,
                              ForeignPhysicalKind kind) -> const auto & {
    const auto &layout = requireLayout(layouts, name);
    CHECK(layout.parameters.size() == 1);
    CHECK(layout.parameters[0].kind == ForeignPassKind::Direct);
    CHECK(layout.parameters[0].lanes.size() == 1);
    CHECK(layout.parameters[0].lanes[0].kind == kind);
    CHECK(layout.result.kind == ForeignPassKind::Direct);
    CHECK(layout.result.lanes == layout.parameters[0].lanes);
    return layout;
  };
  directLane("i64_box", ForeignPhysicalKind::Integer);
  directLane("f64_box", ForeignPhysicalKind::Float64);
  directLane("f32_pair", ForeignPhysicalKind::Float32Vector2);
  directLane("number", ForeignPhysicalKind::Integer);

  for (const auto name :
       {std::string_view("mixed"), std::string_view("nested")}) {
    const auto &layout = requireLayout(layouts, name);
    CHECK(layout.parameters[0].kind == ForeignPassKind::Direct);
    CHECK(layout.parameters[0].lanes.size() == 2);
    CHECK(layout.parameters[0].lanes[0].kind ==
          (name == "mixed" ? ForeignPhysicalKind::Integer
                           : ForeignPhysicalKind::Float32Vector2));
    CHECK(layout.parameters[0].lanes[1].kind ==
          (name == "mixed" ? ForeignPhysicalKind::Float64
                           : ForeignPhysicalKind::Integer));
    CHECK(layout.result.lanes == layout.parameters[0].lanes);
  }

  const auto &array = requireLayout(layouts, "array_box");
  CHECK(array.parameters[0].kind == ForeignPassKind::Direct);
  CHECK(array.parameters[0].lanes.size() == 2);
  CHECK(array.parameters[0].lanes[0].kind == ForeignPhysicalKind::Integer);
  CHECK(array.parameters[0].lanes[1].kind == ForeignPhysicalKind::Integer);

  const auto &large_layout = requireLayout(layouts, "large");
  CHECK(large_layout.parameters[0].kind == ForeignPassKind::Indirect);
  CHECK(large_layout.parameters[0].by_value);
  CHECK(large_layout.result.kind == ForeignPassKind::Indirect);
  CHECK(!large_layout.result.by_value);

  auto llvm = lowerToLLVM(low_ir, "sysv-abi", "sysv_abi",
                          "x86_64-unknown-linux-gnu", error);
  if (!error.empty())
    std::fprintf(stderr, "SysV LLVM lowering failed: %s\n", error.c_str());
  CHECK(error.empty());
  CHECK(llvm.verify(error));
  const auto text = llvm.print();
  CHECK(text.find("target triple = \"x86_64-unknown-linux-gnu\"") !=
        std::string::npos);
  CHECK(text.find("i8 signext") != std::string::npos);
  CHECK(text.find("i16 zeroext") != std::string::npos);
  CHECK(text.find("byval") != std::string::npos);
  CHECK(text.find("sret") != std::string::npos);
  CHECK(text.find("x86_64-pc-windows-msvc") == std::string::npos);
  CHECK(text.find("inalloca") == std::string::npos);
  return 0;
}
