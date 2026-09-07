#include "chtholly/Compiler/PublicInterface.h"
#include "chtholly/Compiler/SharedValueStores.h"

#include <sstream>

namespace chtholly::compiler {

std::string publicTypeName(PublicType type) {
  switch (type.kind) {
  case PublicTypeKind::Void:
    return "void";
  case PublicTypeKind::Never:
    return "never";
  case PublicTypeKind::Bool:
    return "bool";
  case PublicTypeKind::Char:
    return "char";
  case PublicTypeKind::Integer:
    return std::string(type.integer_signed ? "i" : "u") +
           std::to_string(type.scalar_width);
  case PublicTypeKind::Float:
    return "f" + std::to_string(type.scalar_width);
  case PublicTypeKind::String:
    return "string";
  case PublicTypeKind::TypeParameter:
    return "type-parameter" + std::to_string(type.binding_index);
  case PublicTypeKind::TypeProjection:
    if (type.arguments.size() != 1)
      return "invalid-type-projection";
    if (type.projection_kind == PublicTypeProjectionKind::Associated)
      return publicTypeName(type.arguments.front()) + " as " +
             type.nominal_entity.canonical_name + "::binding" +
             std::to_string(type.projection_index);
    return publicTypeName(type.arguments.front()) +
           (type.projection_kind == PublicTypeProjectionKind::Pointee
                ? "::pointee"
                : "::element" + std::to_string(type.projection_index));
  case PublicTypeKind::Nominal: {
    std::string result = type.nominal_entity.canonical_name;
    if (!type.arguments.empty()) {
      result += '<';
      for (std::size_t index = 0; index < type.arguments.size(); ++index) {
        if (index != 0)
          result += ", ";
        result += publicTypeName(type.arguments[index]);
      }
      result += '>';
    }
    return result;
  }
  case PublicTypeKind::Reference: {
    if (type.arguments.size() != 1)
      return "invalid-reference";
    std::string result =
        type.reference_mutability == PublicReferenceMutability::ReadOnly
            ? "const "
            : "";
    result += publicTypeName(type.arguments.front());
    result += '&';
    if (type.reference_provenance.kind ==
        PublicReferenceProvenanceKind::Parameter)
      result +=
          "[parameter" + std::to_string(type.reference_provenance.index) + ']';
    return result;
  }
  case PublicTypeKind::RawPointer:
    return type.arguments.size() == 1
               ? std::string(type.pointer_const ? "const " : "") +
                     publicTypeName(type.arguments.front()) + "*"
               : "invalid-pointer";
  case PublicTypeKind::Array:
    return type.arguments.size() == 1
               ? publicTypeName(type.arguments.front()) + "[" +
                     std::to_string(type.array_bound) + "]"
               : "invalid-array";
  case PublicTypeKind::Tuple: {
    std::string result = "(";
    for (std::size_t index = 0; index < type.arguments.size(); ++index) {
      if (index != 0)
        result += ", ";
      result += publicTypeName(type.arguments[index]);
    }
    return result + ")";
  }
  case PublicTypeKind::Slice:
    return type.arguments.size() == 1
               ? std::string(type.slice_mutable ? "mut slice<" : "slice<") +
                     publicTypeName(type.arguments.front()) + ">"
               : "invalid-slice";
  case PublicTypeKind::CFunctionPointer: {
    if (type.arguments.empty())
      return "invalid-c-function-pointer";
    std::string result = "unsafe extern \"C\" fn(";
    for (std::size_t index = 0; index + 1 < type.arguments.size(); ++index) {
      if (index != 0)
        result += ", ";
      if (index == type.callable_context_parameter)
        result += "context ";
      result += publicTypeName(type.arguments[index]);
    }
    if (type.callable_variadic)
      result += type.arguments.size() > 1 ? ", ..." : "...";
    result += "): " + publicTypeName(type.arguments.back());
    return result;
  }
  case PublicTypeKind::Function: {
    if (type.arguments.empty())
      return "invalid-function";
    std::string result = "fn(";
    for (std::size_t index = 0; index + 1 < type.arguments.size(); ++index) {
      if (index != 0)
        result += ", ";
      result += publicTypeName(type.arguments[index]);
    }
    result += "): " + publicTypeName(type.arguments.back());
    return result;
  }
  case PublicTypeKind::CallbackAdapter:
    return type.arguments.size() == 3
               ? "callback adapter { entry: " +
                     publicTypeName(type.arguments[0]) + "; context: owned " +
                     publicTypeName(type.arguments[1]) +
                     "; release: " + publicTypeName(type.arguments[2]) + "; }"
               : "invalid-callback-adapter";
  case PublicTypeKind::CallbackRegistration:
    return (type.arguments.size() == 5 || type.arguments.size() == 7 ||
            type.arguments.size() == 8 || type.arguments.size() == 10)
               ? "callback registration { callback: " +
                     publicTypeName(type.arguments[0]) +
                     "; handle: " + publicTypeName(type.arguments[1]) +
                     "; register: " + publicTypeName(type.arguments[2]) +
                     "; unregister: " + publicTypeName(type.arguments[3]) +
                     "; cancel: " + publicTypeName(type.arguments[4]) +
                     (type.arguments.size() >= 7
                          ? "; cancel_async: " +
                                publicTypeName(type.arguments[5]) +
                                "; wait: " + publicTypeName(type.arguments[6])
                          : "") +
                     (type.arguments.size() >= 8
                          ? "; poll: " + publicTypeName(type.arguments[7])
                          : "") +
                     (type.arguments.size() == 10
                          ? "; arm: " + publicTypeName(type.arguments[8]) +
                                "; detach: " + publicTypeName(type.arguments[9])
                          : "") +
                     " }"
               : "invalid-callback-registration";
  case PublicTypeKind::CallbackCompletion:
    return (type.arguments.size() == 4 || type.arguments.size() == 5 ||
            type.arguments.size() == 7)
               ? "callback completion { callback: " +
                     publicTypeName(type.arguments[0]) +
                     "; handle: " + publicTypeName(type.arguments[1]) +
                     "; token: " + publicTypeName(type.arguments[2]) +
                     "; wait: " + publicTypeName(type.arguments[3]) +
                     (type.arguments.size() >= 5
                          ? "; poll: " + publicTypeName(type.arguments[4])
                          : "") +
                     (type.arguments.size() == 7
                          ? "; arm: " + publicTypeName(type.arguments[5]) +
                                "; detach: " + publicTypeName(type.arguments[6])
                          : "") +
                     " }"
               : "invalid-callback-completion";
  case PublicTypeKind::CallbackWake:
    return type.arguments.size() == 1
               ? "callback wake { completion: " +
                     publicTypeName(type.arguments.front()) + "; }"
               : "invalid-callback-wake";
  case PublicTypeKind::ForeignCompletion:
    return type.nominal_entity.canonical_name + "::Completion";
  case PublicTypeKind::ForeignWake:
    return type.nominal_entity.canonical_name + "::Wake";
  case PublicTypeKind::Count:
    return "invalid";
  }
  return "invalid";
}

std::string PublicInterfaceArtifact::print() const {
  std::ostringstream out;
  out << "artifact " << package_name_ << '/' << module_name_
      << " fingerprint=" << fingerprint_.hex() << ":\n";
  for (const auto &function : functions_) {
    out << "  " << function.name << " -> " << function.canonical_package << '/'
        << function.canonical_module << "::" << function.canonical_name << '(';
    bool first = true;
    for (const auto parameter : function.parameters) {
      if (!first)
        out << ", ";
      first = false;
      out << publicTypeName(parameter);
    }
    out << ") -> " << publicTypeName(function.return_type) << ' '
        << function.entity_fingerprint.hex() << '\n';
  }
  for (const auto &nominal : nominal_types_)
    out << "  type " << nominal.entity.canonical_name
        << " fields=" << nominal.fields.size() << ' '
        << nominal.definition_fingerprint.hex() << '\n';
  for (const auto &value : values_)
    out << "  "
        << (value.kind == PublicValueKind::Static ? "static " : "const ")
        << value.name << " : " << publicTypeName(value.type) << ' '
        << value.entity_fingerprint.hex() << '\n';
  return out.str();
}

std::string PublicInterface::print() const {
  std::ostringstream out;
  out << "public ";
  if (check_ir_id_.hasValue())
    out << "check_ir" << check_ir_id_.index << ' ';
  else
    out << "external ";
  out << values_->identifier(package_name_) << '/'
      << values_->identifier(module_name_)
      << " fingerprint=" << fingerprint_.hex() << ":\n";
  for (std::uint32_t index = 0; index < functions_.size(); ++index) {
    const auto &value = function(PublicBindingId(index));
    out << "  binding" << index << (value.member_owner ? " method " : " fn ")
        << values_->identifier(value.name) << '(';
    bool first = true;
    for (const auto parameter : parameterTypes(value.parameters)) {
      if (!first)
        out << ", ";
      first = false;
      out << publicTypeName(parameter);
    }
    out << ") -> " << publicTypeName(value.return_type) << " entity"
        << value.canonical_entity.index << '\n';
  }
  for (std::uint32_t index = 0; index < nominal_types_.size(); ++index) {
    const auto &value = nominalType(PublicBindingId(index));
    out << "  binding-type" << index << ' ' << values_->identifier(value.name)
        << " entity" << value.canonical_entity.index << '\n';
  }
  for (const auto &value : value_artifacts_)
    out << "  "
        << (value.kind == PublicValueKind::Static ? "static " : "const ")
        << value.name << " : " << publicTypeName(value.type) << ' '
        << value.entity_fingerprint.hex() << '\n';
  return out.str();
}

} // namespace chtholly::compiler
