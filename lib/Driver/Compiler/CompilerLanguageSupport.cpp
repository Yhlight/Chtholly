#include "chtholly/Driver/CompilerLanguageSupport.h"

#include "chtholly/Driver/CompilerInputFileSystem.h"
#include "chtholly/Compiler/CFDL.h"
#include "chtholly/Compiler/CompilationUnit.h"
#include "chtholly/Compiler/ParseTree.h"
#include "chtholly/Compiler/SemIR.h"
#include "chtholly/Compiler/TokenBuffer.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <limits>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace chtholly {
namespace {

struct DecodedCodePoint {
  std::uint32_t value = 0;
  std::size_t length = 0;
};

std::optional<DecodedCodePoint> decodeUtf8(std::string_view text,
                                           std::size_t offset) {
  if (offset >= text.size())
    return std::nullopt;
  const auto first = static_cast<unsigned char>(text[offset]);
  if (first < 0x80)
    return DecodedCodePoint{first, 1};
  std::size_t length = 0;
  std::uint32_t value = 0;
  if ((first & 0xE0U) == 0xC0U) {
    length = 2;
    value = first & 0x1FU;
  } else if ((first & 0xF0U) == 0xE0U) {
    length = 3;
    value = first & 0x0FU;
  } else if ((first & 0xF8U) == 0xF0U) {
    length = 4;
    value = first & 0x07U;
  } else {
    return std::nullopt;
  }
  if (offset + length > text.size())
    return std::nullopt;
  for (std::size_t index = 1; index < length; ++index) {
    const auto byte = static_cast<unsigned char>(text[offset + index]);
    if ((byte & 0xC0U) != 0x80U)
      return std::nullopt;
    value = (value << 6U) | (byte & 0x3FU);
  }
  const auto minimum = length == 2 ? 0x80U : length == 3 ? 0x800U : 0x10000U;
  if (value < minimum || value > 0x10FFFFU ||
      (value >= 0xD800U && value <= 0xDFFFU))
    return std::nullopt;
  return DecodedCodePoint{value, length};
}

bool validateUtf8(std::string_view text, std::string &error) {
  for (std::size_t offset = 0; offset < text.size();) {
    const auto decoded = decodeUtf8(text, offset);
    if (!decoded) {
      error = "compiler document contains invalid UTF-8";
      return false;
    }
    offset += decoded->length;
  }
  return true;
}

std::uint32_t utf16Width(std::uint32_t code_point) {
  return code_point > 0xFFFFU ? 2U : 1U;
}

std::string typeText(const compiler::SemIR &sem_ir, compiler::TypeId id) {
  if (!id.hasValue())
    return "<invalid>";
  const auto &type = sem_ir.type(id);
  switch (type.kind) {
  case compiler::SemTypeKind::Void:
    return "void";
  case compiler::SemTypeKind::Bool:
    return "bool";
  case compiler::SemTypeKind::Integer:
    return std::string(type.arg1 != 0 ? "i" : "u") + std::to_string(type.arg0);
  case compiler::SemTypeKind::Float:
    return "f" + std::to_string(type.arg0);
  case compiler::SemTypeKind::String:
    return "string";
  case compiler::SemTypeKind::Array:
    return typeText(sem_ir, compiler::TypeId(type.arg0)) + "[" +
           std::to_string(type.arg1) + "]";
  case compiler::SemTypeKind::RawPointer:
    return std::string(sem_ir.rawPointerPointeeConst(id) ? "const " : "") +
           typeText(sem_ir, sem_ir.rawPointerPointee(id)) + "*";
  case compiler::SemTypeKind::Function: {
    std::string result = "fn(";
    bool first = true;
    for (const auto parameter :
         sem_ir.typeBlock(compiler::TypeBlockId(type.arg0))) {
      if (!first)
        result += ", ";
      first = false;
      result += typeText(sem_ir, parameter);
    }
    return result + "): " + typeText(sem_ir, compiler::TypeId(type.arg1));
  }
  case compiler::SemTypeKind::Nominal: {
    const auto &nominal = sem_ir.nominalType(compiler::NominalTypeId(type.arg0));
    std::string result(sem_ir.identifier(sem_ir.name(nominal.name).text));
    const auto arguments = sem_ir.typeBlock(compiler::TypeBlockId(type.arg1));
    if (!arguments.empty()) {
      result += "<";
      for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (index != 0)
          result += ", ";
        result += typeText(sem_ir, arguments[index]);
      }
      result += ">";
    }
    return result;
  }
  case compiler::SemTypeKind::Reference:
    return std::string(type.arg1 == 0 ? "const " : "") +
           typeText(sem_ir, compiler::TypeId(type.arg0)) + "&";
  case compiler::SemTypeKind::TypeParameter:
    return "T" + std::to_string(type.arg1);
  case compiler::SemTypeKind::Invalid:
  case compiler::SemTypeKind::Count:
    return "<invalid>";
  }
  return "<invalid>";
}

std::string publicConstantText(const compiler::PublicConstantValue &value) {
  switch (value.kind) {
  case compiler::PublicConstantValueKind::Integer:
    return std::to_string(std::bit_cast<std::int64_t>(value.payload));
  case compiler::PublicConstantValueKind::Float:
    return std::to_string(std::bit_cast<double>(value.payload));
  case compiler::PublicConstantValueKind::Bool:
    return value.payload == 0 ? "false" : "true";
  case compiler::PublicConstantValueKind::String:
    return "\"" + value.string_payload + "\"";
  case compiler::PublicConstantValueKind::Null:
    return "null";
  default:
    return "<constant>";
  }
}

std::string publicTypeText(const compiler::PublicType &type) {
  using Kind = compiler::PublicTypeKind;
  switch (type.kind) {
  case Kind::Void:
    return "void";
  case Kind::Bool:
    return "bool";
  case Kind::Integer:
    return std::string(type.integer_signed ? "i" : "u") +
           std::to_string(type.scalar_width);
  case Kind::Float:
    return "f" + std::to_string(type.scalar_width);
  case Kind::String:
    return "string";
  case Kind::Function: {
    if (type.arguments.empty())
      return "fn(<invalid>): <invalid>";
    std::string result = "fn(";
    for (std::size_t index = 0; index + 1 < type.arguments.size(); ++index) {
      if (index != 0)
        result += ", ";
      result += publicTypeText(type.arguments[index]);
    }
    return result + "): " + publicTypeText(type.arguments.back());
  }
  case Kind::TypeParameter:
    return "T" + std::to_string(type.binding_index);
  case Kind::Nominal: {
    std::string result = type.nominal_entity.canonical_name;
    if (!type.arguments.empty()) {
      result += "<";
      for (std::size_t index = 0; index < type.arguments.size(); ++index) {
        if (index != 0)
          result += ", ";
        result += publicTypeText(type.arguments[index]);
      }
      result += ">";
    }
    return result;
  }
  case Kind::Reference:
    return std::string(type.reference_mutability ==
                               compiler::PublicReferenceMutability::ReadOnly
                           ? "const "
                           : "") +
           (type.arguments.empty() ? "<invalid>"
                                   : publicTypeText(type.arguments.front())) +
           "&";
  case Kind::RawPointer:
    return std::string(type.pointer_const ? "const " : "") +
           (type.arguments.empty() ? "<invalid>"
                                   : publicTypeText(type.arguments.front())) +
           "*";
  case Kind::Array:
    return (type.arguments.empty() ? std::string("<invalid>")
                                   : publicTypeText(type.arguments.front())) +
           "[" + std::to_string(type.array_bound) + "]";
  default:
    return "<callable>";
  }
}

std::string publicFunctionSignature(const compiler::PublicEntity &entity,
                                    std::string_view display_name) {
  std::string result = "fn " + std::string(display_name) + "(";
  for (std::size_t index = 0; index < entity.parameters.size(); ++index) {
    if (index != 0)
      result += ", ";
    if (index < entity.parameter_names.size())
      result += entity.parameter_names[index] + ": ";
    result += publicTypeText(entity.parameters[index]);
    if (index < entity.default_arguments.size() &&
        entity.default_arguments[index])
      result += " = " + publicConstantText(*entity.default_arguments[index]);
  }
  return result + "): " + publicTypeText(entity.return_type);
}

std::optional<compiler::PublicEntityReferenceArtifact>
nominalEntityReference(const compiler::SemIR &sem_ir,
                       const compiler::SemNominalType &nominal,
                       const compiler::PublicInterface *interface);

const compiler::PublicEntity *
callablePublicEntity(const compiler::SemIR &sem_ir,
                     const compiler::SemFunctionRef &reference,
                     const compiler::PublicInterface *interface) {
  if (const auto *entity =
          sem_ir.importIRs().tryGetEntity(reference.public_entity))
    return entity;
  if (!reference.local_function.hasValue() || !interface)
    return nullptr;
  const auto &function = sem_ir.function(reference.local_function);
  if ((function.flags & compiler::SemFunctionPublic) == 0)
    return nullptr;
  const compiler::SemFunctionRef *identity_reference = &reference;
  if (reference.generic.hasValue()) {
    const auto self_specific =
        sem_ir.genericValues().generic(reference.generic).self_specific;
    for (std::uint32_t index = 0; index < sem_ir.functionRefCount(); ++index) {
      const auto &candidate = sem_ir.functionRef(compiler::FunctionRefId(index));
      if (candidate.generic == reference.generic &&
          candidate.specific == self_specific) {
        identity_reference = &candidate;
        break;
      }
    }
  }
  const auto &local_type = sem_ir.type(identity_reference->local_type);
  if (local_type.kind != compiler::SemTypeKind::Function)
    return nullptr;
  const auto local_parameters =
      sem_ir.typeBlock(compiler::TypeBlockId(local_type.arg0));
  const auto name = sem_ir.name(function.name).text;
  std::span<const compiler::PublicBindingId> bindings;
  std::optional<compiler::PublicEntityReferenceArtifact> owner;
  if (function.semantic_owner.hasValue()) {
    owner = nominalEntityReference(
        sem_ir, sem_ir.nominalType(function.semantic_owner), interface);
    if (owner)
      bindings = interface->findMemberFunctions(*owner, name);
  } else {
    bindings = interface->findFunctions(name);
  }
  const auto generic_count =
      function.generic.hasValue()
          ? sem_ir.genericValues().generic(function.generic).binding_count
          : 0U;
  auto expected_member_kind = compiler::PublicFunctionArtifact::MemberKind::None;
  if (function.semantic_owner.hasValue()) {
    bool found_member_kind = false;
    for (const auto &member :
         sem_ir.nominalType(function.semantic_owner).member_functions) {
      const auto &target = sem_ir.functionRef(member.target);
      const auto matches =
          reference.generic.hasValue()
              ? target.generic == reference.generic
              : target.local_function == identity_reference->local_function;
      if (!matches)
        continue;
      expected_member_kind =
          (member.flags & compiler::SemNominalMemberFunctionAssociated) != 0
              ? compiler::PublicFunctionArtifact::MemberKind::Associated
              : compiler::PublicFunctionArtifact::MemberKind::Instance;
      found_member_kind = true;
      break;
    }
    if (!found_member_kind)
      return nullptr;
  }
  const compiler::PublicEntity *result = nullptr;
  const auto matches_tooling_type = [&](compiler::TypeId local,
                                        const compiler::PublicType &external) {
    return sem_ir.matchesPublicType(local, external) ||
           typeText(sem_ir, local) == publicTypeText(external);
  };
  for (const auto binding_id : bindings) {
    const auto &binding = interface->function(binding_id);
    const auto *entity =
        sem_ir.importIRs().registry().tryGetEntity(binding.canonical_entity);
    if (!entity || entity->generic_parameter_count != generic_count ||
        entity->member_kind != expected_member_kind ||
        entity->parameters.size() != local_parameters.size() ||
        !matches_tooling_type(compiler::TypeId(local_type.arg1),
                              entity->return_type))
      continue;
    bool matches = true;
    for (std::size_t index = 0; index < local_parameters.size(); ++index)
      matches &= matches_tooling_type(local_parameters[index],
                                      entity->parameters[index]);
    if (!matches)
      continue;
    if (result)
      return nullptr;
    result = entity;
  }
  return result;
}

std::optional<compiler::PublicEntityReferenceArtifact>
nominalEntityReference(const compiler::SemIR &sem_ir,
                       const compiler::SemNominalType &nominal,
                       const compiler::PublicInterface *interface) {
  if (const auto *entity =
          sem_ir.importIRs().tryGetEntity(nominal.canonical_entity)) {
    return compiler::PublicEntityReferenceArtifact{
        compiler::PublicEntityKind::NominalType,
        std::string(sem_ir.identifier(entity->package_name)),
        std::string(sem_ir.identifier(entity->module_name)),
        std::string(sem_ir.identifier(entity->name)), entity->fingerprint};
  }
  if (!interface)
    return std::nullopt;
  const auto name = sem_ir.identifier(sem_ir.name(nominal.name).text);
  const auto found = std::ranges::find(
      interface->nominalArtifacts(), name,
      [](const compiler::PublicNominalTypeArtifact &artifact) {
        return std::string_view(artifact.entity.canonical_name);
      });
  return found == interface->nominalArtifacts().end()
             ? std::nullopt
             : std::optional(found->entity);
}

std::string callableEntityKey(const compiler::SemIR &sem_ir,
                              const compiler::SemFunctionRef &reference,
                              const compiler::PublicInterface *interface,
                              std::string_view local_scope) {
  if (const auto *entity = callablePublicEntity(sem_ir, reference, interface))
    return "entity:" + entity->fingerprint.hex();
  if (!reference.local_function.hasValue())
    return {};
  return "function:" + std::string(local_scope) + ":" +
         std::to_string(reference.local_function.index);
}

std::string functionSignature(const compiler::SemIR &sem_ir,
                              const compiler::SemFunctionRef &reference,
                              std::string_view name) {
  const auto &type = sem_ir.type(reference.local_type);
  std::string result = "```chtholly\nfn " + std::string(name) + "(";
  const compiler::SemCallableDeclaration *declaration =
      reference.local_function.hasValue()
          ? &sem_ir.functionDeclaration(reference.local_function)
          : nullptr;
  bool first = true;
  std::size_t index = 0;
  for (const auto parameter : sem_ir.typeBlock(compiler::TypeBlockId(type.arg0))) {
    if (!first)
      result += ", ";
    first = false;
    if (declaration && index < declaration->parameter_names.size())
      result +=
          std::string(sem_ir.identifier(declaration->parameter_names[index])) +
          ": ";
    result += typeText(sem_ir, parameter);
    if (declaration && index < declaration->default_arguments.size() &&
        declaration->default_arguments[index].hasValue())
      result += " = <constant>";
    ++index;
  }
  return result + "): " + typeText(sem_ir, compiler::TypeId(type.arg1)) + "\n```";
}

void mergeCompletionOverloads(std::vector<CompilerCompletionItem> &items) {
  std::ranges::sort(items, [](const auto &left, const auto &right) {
    return std::tie(left.label, left.kind, left.detail) <
           std::tie(right.label, right.kind, right.detail);
  });
  items.erase(std::unique(items.begin(), items.end()), items.end());
  std::vector<CompilerCompletionItem> merged;
  for (std::size_t begin = 0; begin < items.size();) {
    auto end = begin + 1;
    while (end < items.size() && items[end].label == items[begin].label &&
           items[end].kind == items[begin].kind)
      ++end;
    auto item = items[begin];
    if (end - begin > 1) {
      item.detail = std::to_string(end - begin) + " overloads";
      for (auto index = begin; index < end; ++index)
        item.detail += "\n" + items[index].detail;
    }
    merged.push_back(std::move(item));
    begin = end;
  }
  items = std::move(merged);
}

} // namespace

bool nextTextPositionToOffset(std::string_view text, CompilerTextPosition position,
                              std::uint32_t &offset, std::string &error) {
  error.clear();
  if (!validateUtf8(text, error))
    return false;
  std::size_t line_start = 0;
  for (std::uint32_t line = 0; line < position.line; ++line) {
    const auto newline = text.find_first_of("\r\n", line_start);
    if (newline == std::string_view::npos) {
      error = "compiler text position line is out of range";
      return false;
    }
    line_start =
        newline + (text[newline] == '\r' && newline + 1 < text.size() &&
                           text[newline + 1] == '\n'
                       ? 2
                       : 1);
  }
  const auto newline = text.find_first_of("\r\n", line_start);
  const auto line_end =
      newline == std::string_view::npos ? text.size() : newline;
  std::uint32_t character = 0;
  auto cursor = line_start;
  while (cursor < line_end && character < position.character) {
    const auto decoded = decodeUtf8(text, cursor);
    if (!decoded)
      return false;
    const auto width = utf16Width(decoded->value);
    if (character + width > position.character) {
      error = "compiler text position splits a UTF-16 surrogate pair";
      return false;
    }
    character += width;
    cursor += decoded->length;
  }
  if (character != position.character) {
    error = "compiler text position character is out of range";
    return false;
  }
  if (cursor > std::numeric_limits<std::uint32_t>::max()) {
    error = "compiler document exceeds the supported offset range";
    return false;
  }
  offset = static_cast<std::uint32_t>(cursor);
  return true;
}

bool compilerTextOffsetToPosition(std::string_view text, std::uint32_t offset,
                              CompilerTextPosition &position, std::string &error) {
  error.clear();
  if (offset > text.size()) {
    error = "compiler text offset is out of range";
    return false;
  }
  if (!validateUtf8(text, error))
    return false;
  position = {};
  for (std::size_t cursor = 0; cursor < offset;) {
    if (text[cursor] == '\r') {
      if (cursor + 1 < text.size() && text[cursor + 1] == '\n') {
        if (cursor + 1 == offset) {
          error = "compiler text offset is inside a CRLF sequence";
          return false;
        }
        cursor += 2;
      } else {
        ++cursor;
      }
      ++position.line;
      position.character = 0;
      continue;
    }
    if (text[cursor] == '\n') {
      ++cursor;
      ++position.line;
      position.character = 0;
      continue;
    }
    const auto decoded = decodeUtf8(text, cursor);
    if (!decoded || cursor + decoded->length > offset) {
      error = "compiler text offset is inside a UTF-8 code point";
      return false;
    }
    position.character += utf16Width(decoded->value);
    cursor += decoded->length;
  }
  return true;
}

bool applyNextTextChanges(
    std::string_view current,
    std::span<const CompilerTextDocumentContentChange> changes, std::string &next,
    std::string &error) {
  error.clear();
  std::string working(current);
  if (!validateUtf8(working, error))
    return false;
  for (const auto &change : changes) {
    if (!validateUtf8(change.text, error))
      return false;
    if (!change.range) {
      working = change.text;
      continue;
    }
    std::uint32_t start = 0;
    std::uint32_t end = 0;
    if (!nextTextPositionToOffset(working, change.range->start, start, error) ||
        !nextTextPositionToOffset(working, change.range->end, end, error))
      return false;
    if (start > end) {
      error = "compiler text edit range start follows its end";
      return false;
    }
    if (change.range_length) {
      std::uint32_t units = 0;
      for (auto cursor = start; cursor < end;) {
        const auto decoded = decodeUtf8(working, cursor);
        if (!decoded) {
          error = "compiler text edit range contains invalid UTF-8";
          return false;
        }
        units += utf16Width(decoded->value);
        cursor += static_cast<std::uint32_t>(decoded->length);
      }
      if (units != *change.range_length) {
        error = "compiler text edit rangeLength does not match its UTF-16 range";
        return false;
      }
    }
    working.replace(start, end - start, change.text);
  }
  next = std::move(working);
  return true;
}

struct CompilerWorkspaceSymbolIndex::Impl {
  struct Entity {
    std::string hover;
    std::string name;
    CompilerDocumentSymbolKind symbol_kind = CompilerDocumentSymbolKind::Function;
    bool document_symbol = false;
    std::optional<CompilerSourceLocation> declaration;
  };
  struct Occurrence {
    std::string key;
    CompilerSourceLocation location;
    bool declaration = false;
  };
  struct CompletionContext {
    CompilerSourceLocation location;
    std::vector<CompilerCompletionItem> items;
  };

  std::unordered_map<std::string, std::string> source_text;
  std::unordered_map<std::string, Entity> entities;
  std::vector<Occurrence> occurrences;
  std::vector<CompletionContext> completion_contexts;

  const Occurrence *find(std::string_view path,
                         CompilerTextPosition position) const {
    const auto normalized = normalizeCompilerInputPath(path);
    const auto source = source_text.find(normalized);
    if (source == source_text.end())
      return nullptr;
    std::uint32_t offset = 0;
    std::string error;
    if (!nextTextPositionToOffset(source->second, position, offset, error))
      return nullptr;
    for (const auto &occurrence : occurrences) {
      if (occurrence.location.path != normalized)
        continue;
      std::uint32_t start = 0;
      std::uint32_t end = 0;
      if (nextTextPositionToOffset(
              source->second, occurrence.location.range.start, start, error) &&
          nextTextPositionToOffset(source->second,
                                   occurrence.location.range.end, end, error) &&
          offset >= start && offset < end)
        return &occurrence;
    }
    return nullptr;
  }
};

CompilerWorkspaceSymbolIndex::CompilerWorkspaceSymbolIndex(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
CompilerWorkspaceSymbolIndex::CompilerWorkspaceSymbolIndex(
    CompilerWorkspaceSymbolIndex &&) noexcept = default;
CompilerWorkspaceSymbolIndex &CompilerWorkspaceSymbolIndex::operator=(
    CompilerWorkspaceSymbolIndex &&) noexcept = default;
CompilerWorkspaceSymbolIndex::~CompilerWorkspaceSymbolIndex() = default;

std::shared_ptr<const CompilerWorkspaceSymbolIndex> CompilerWorkspaceSymbolIndex::build(
    std::span<const std::shared_ptr<const compiler::CompilationSession>> sessions) {
  auto impl = std::make_unique<Impl>();
  for (const auto &session : sessions) {
    if (!session)
      continue;
    for (std::uint32_t unit_index = 0; unit_index < session->unitCount();
         ++unit_index) {
      const auto &unit = session->unit(compiler::CheckIRId(unit_index));
      const auto path = normalizeCompilerInputPath(unit.sourcePath());
      impl->source_text[path] = std::string(unit.source().text());
      if (const auto *cfdl = unit.cfdlSyntax()) {
        CompilerTextPosition end;
        std::string position_error;
        if (compilerTextOffsetToPosition(
                unit.source().text(),
                static_cast<std::uint32_t>(unit.source().text().size()), end,
                position_error)) {
          std::vector<CompilerCompletionItem> items;
          const auto add = [&](std::string label, std::string detail) {
            if (std::ranges::none_of(items, [&](const auto &item) {
                  return item.label == label;
                }))
              items.push_back({.label = std::move(label),
                               .detail = std::move(detail),
                               .kind = CompilerCompletionItemKind::Function});
          };
          for (auto candidate : compiler::cfdlCompletionCandidates(*cfdl))
            add(std::move(candidate), "CFDL operation capability");
          std::ranges::sort(items, {}, &CompilerCompletionItem::label);
          impl->completion_contexts.push_back(
              {{.path = path, .range = {.start = {}, .end = end}},
               std::move(items)});
        }
        continue;
      }
      const auto *tree = unit.parseTree();
      const auto *sem_ir = unit.semIR();
      if (!tree || !sem_ir)
        continue;
      const auto *interface = unit.publicInterface();
      for (const auto &occurrence : sem_ir->symbolOccurrences()) {
        const auto token_id = tree->token(occurrence.location);
        const auto &token = tree->tokens().get(token_id);
        CompilerTextPosition start;
        CompilerTextPosition end;
        std::string range_error;
        if (!compilerTextOffsetToPosition(unit.source().text(), token.offset, start,
                                      range_error) ||
            !compilerTextOffsetToPosition(
                unit.source().text(),
                token.offset + std::max<std::uint32_t>(1U, token.length), end,
                range_error))
          continue;

        std::string key;
        std::string hover;
        std::string symbol_name;
        auto symbol_kind = CompilerDocumentSymbolKind::Function;
        bool document_symbol = false;
        if (occurrence.target_kind == compiler::SemSymbolTargetKind::Local) {
          const auto local_id = compiler::LocalId(occurrence.target);
          const auto &local = sem_ir->local(local_id);
          const auto name = sem_ir->identifier(sem_ir->name(local.name).text);
          symbol_name = name;
          key = "local:" + path + ":" + std::to_string(local_id.index);
          hover = "```chtholly\n" + std::string(name) + ": " +
                  typeText(*sem_ir, local.type) + "\n```";
        } else if (occurrence.target_kind ==
                   compiler::SemSymbolTargetKind::Constant) {
          const auto constant_id = compiler::ConstantEntityId(occurrence.target);
          const auto &constant = sem_ir->constantEntity(constant_id);
          const auto name =
              sem_ir->identifier(sem_ir->name(constant.name).text);
          const auto is_static =
              (constant.flags & compiler::SemConstantStatic) != 0;
          symbol_name = name;
          symbol_kind = is_static ? CompilerDocumentSymbolKind::Static
                                  : CompilerDocumentSymbolKind::Constant;
          document_symbol = true;
          if (constant.public_fingerprint.hasValue()) {
            key = "entity:" + constant.public_fingerprint.hex();
          } else if (interface &&
                     (constant.flags & compiler::SemConstantPublic) != 0) {
            const auto *artifact =
                interface->findValue(sem_ir->name(constant.name).text);
            key = artifact ? "entity:" + artifact->entity_fingerprint.hex()
                           : std::string{};
          }
          if (key.empty())
            key = "constant:" + path + ":" + std::to_string(constant_id.index);
          hover = "```chtholly\n" +
                  std::string(is_static ? "static " : "const ") +
                  std::string(name) + ": " + typeText(*sem_ir, constant.type) +
                  "\n```";
        } else {
          const auto function_ref_id = compiler::FunctionRefId(occurrence.target);
          const auto &reference = sem_ir->functionRef(function_ref_id);
          std::string_view name;
          if (reference.local_function.hasValue()) {
            const auto &function = sem_ir->function(reference.local_function);
            name = sem_ir->identifier(sem_ir->name(function.name).text);
          } else {
            const auto *entity =
                sem_ir->importIRs().tryGetEntity(reference.public_entity);
            if (entity)
              name = sem_ir->identifier(entity->name);
          }
          symbol_name = name;
          symbol_kind = CompilerDocumentSymbolKind::Function;
          document_symbol = true;
          const auto *entity =
              callablePublicEntity(*sem_ir, reference, interface);
          key = callableEntityKey(*sem_ir, reference, interface, path);
          hover = functionSignature(*sem_ir, reference, name);
          if (entity) {
            auto display_name = std::string(sem_ir->identifier(entity->name));
            hover = "```chtholly\n" +
                    publicFunctionSignature(*entity, display_name) + "\n```";
            hover +=
                "\n\n`" +
                std::string(sem_ir->identifier(entity->package_name)) +
                "::" + std::string(sem_ir->identifier(entity->module_name)) +
                "::" + std::string(sem_ir->identifier(entity->name)) + "`";
          }
        }
        auto &indexed_entity = impl->entities[key];
        if (indexed_entity.hover.empty())
          indexed_entity.hover = std::move(hover);
        if (indexed_entity.name.empty()) {
          indexed_entity.name = std::move(symbol_name);
          indexed_entity.symbol_kind = symbol_kind;
          indexed_entity.document_symbol = document_symbol;
        }
        Impl::Occurrence indexed{
            .key = key,
            .location = {.path = path, .range = {.start = start, .end = end}},
            .declaration =
                occurrence.kind == compiler::SemSymbolOccurrenceKind::Declaration};
        if (indexed.declaration)
          indexed_entity.declaration = indexed.location;
        impl->occurrences.push_back(std::move(indexed));
      }
      if (interface) {
        std::unordered_map<std::string, std::vector<const compiler::PublicEntity *>>
            overload_groups;
        for (std::uint32_t binding_index = 0;
             binding_index < interface->bindingCount(); ++binding_index) {
          const auto &binding =
              interface->function(compiler::PublicBindingId(binding_index));
          const auto *entity = session->publicInterfaces().tryGetEntity(
              binding.canonical_entity);
          if (!entity)
            continue;
          std::string group =
              std::string(sem_ir->identifier(binding.name)) + ":" +
              std::to_string(static_cast<unsigned>(binding.member_kind));
          if (binding.member_owner)
            group += ":" + binding.member_owner->expected_fingerprint.hex();
          overload_groups[group].push_back(entity);
        }
        for (const auto &[unused, overloads] : overload_groups) {
          (void)unused;
          if (overloads.size() < 2)
            continue;
          std::string details =
              "\n\nOverloads (" + std::to_string(overloads.size()) + "):";
          for (const auto *entity : overloads)
            details += "\n- `" +
                       publicFunctionSignature(
                           *entity, sem_ir->identifier(entity->name)) +
                       "`";
          for (const auto *entity : overloads) {
            const auto found =
                impl->entities.find("entity:" + entity->fingerprint.hex());
            if (found != impl->entities.end() &&
                found->second.hover.find("\n\nOverloads (") ==
                    std::string::npos)
              found->second.hover += details;
          }
        }
      }
      for (const auto &context : sem_ir->memberAccessContexts()) {
        const auto token_id = tree->token(context.location);
        const auto &token = tree->tokens().get(token_id);
        CompilerTextPosition start;
        CompilerTextPosition end;
        std::string range_error;
        if (!compilerTextOffsetToPosition(unit.source().text(), token.offset, start,
                                      range_error) ||
            !compilerTextOffsetToPosition(unit.source().text(),
                                      token.offset + token.length, end,
                                      range_error))
          continue;
        const auto &nominal = sem_ir->nominalType(context.owner);
        const auto owner_name =
            std::string(sem_ir->identifier(sem_ir->name(nominal.name).text));
        std::vector<CompilerCompletionItem> items;
        for (const auto &member : nominal.member_functions) {
          if (nominal.canonical_entity.hasValue() &&
              (member.flags & compiler::SemNominalMemberFunctionPublic) == 0)
            continue;
          const auto associated =
              (member.flags & compiler::SemNominalMemberFunctionAssociated) != 0;
          if (associated !=
              (context.kind == compiler::SemMemberAccessKind::Associated))
            continue;
          const auto &reference = sem_ir->functionRef(member.target);
          const auto label =
              std::string(sem_ir->identifier(sem_ir->name(member.name).text));
          items.push_back(
              {.label = label,
               .detail = functionSignature(*sem_ir, reference,
                                           owner_name + "::" + label),
               .kind = associated ? CompilerCompletionItemKind::AssociatedFunction
                                  : CompilerCompletionItemKind::InstanceMethod});
        }
        const auto owner = nominalEntityReference(*sem_ir, nominal, interface);
        if (owner) {
          for (std::uint32_t import_index = 0;
               import_index < sem_ir->importIRs().size(); ++import_index) {
            const auto *imported = sem_ir->importIRs().tryGetInterface(
                compiler::ImportIRId(import_index));
            if (!imported)
              continue;
            for (std::uint32_t binding_index = 0;
                 binding_index < imported->bindingCount(); ++binding_index) {
              const auto &binding =
                  imported->function(compiler::PublicBindingId(binding_index));
              const auto wanted =
                  context.kind == compiler::SemMemberAccessKind::Associated
                      ? compiler::PublicFunctionArtifact::MemberKind::Associated
                      : compiler::PublicFunctionArtifact::MemberKind::Instance;
              if (!binding.member_owner || *binding.member_owner != *owner ||
                  binding.member_kind != wanted)
                continue;
              const auto *entity = session->publicInterfaces().tryGetEntity(
                  binding.canonical_entity);
              if (!entity)
                continue;
              const auto label = std::string(sem_ir->identifier(binding.name));
              items.push_back(
                  {.label = label,
                   .detail = publicFunctionSignature(
                       *entity, owner->canonical_name + "::" + label),
                   .kind = wanted == compiler::PublicFunctionArtifact::MemberKind::
                                         Associated
                               ? CompilerCompletionItemKind::AssociatedFunction
                               : CompilerCompletionItemKind::InstanceMethod});
            }
          }
        }
        mergeCompletionOverloads(items);
        impl->completion_contexts.push_back(
            {{.path = path, .range = {.start = start, .end = end}},
             std::move(items)});
      }
      for (const auto &context : sem_ir->moduleAccessContexts()) {
        const auto token_id = tree->token(context.location);
        const auto &token = tree->tokens().get(token_id);
        CompilerTextPosition start;
        CompilerTextPosition end;
        std::string range_error;
        if (!compilerTextOffsetToPosition(unit.source().text(), token.offset, start,
                                      range_error) ||
            !compilerTextOffsetToPosition(unit.source().text(),
                                      token.offset + token.length, end,
                                      range_error))
          continue;
        const auto *imported =
            sem_ir->importIRs().tryGetInterface(context.module);
        if (!imported)
          continue;
        std::vector<CompilerCompletionItem> items;
        for (std::uint32_t binding_index = 0;
             binding_index < imported->bindingCount(); ++binding_index) {
          const auto &binding =
              imported->function(compiler::PublicBindingId(binding_index));
          if (binding.member_owner ||
              binding.member_kind !=
                  compiler::PublicFunctionArtifact::MemberKind::None)
            continue;
          const auto label = std::string(sem_ir->identifier(binding.name));
          if (label.starts_with('$'))
            continue;
          const auto *entity = session->publicInterfaces().tryGetEntity(
              binding.canonical_entity);
          if (!entity)
            continue;
          items.push_back({.label = label,
                           .detail = publicFunctionSignature(*entity, label),
                           .kind = CompilerCompletionItemKind::Function});
        }
        for (const auto &value : imported->valueArtifacts()) {
          const auto is_static = value.kind == compiler::PublicValueKind::Static;
          items.push_back(
              {.label = value.name,
               .detail = std::string(is_static ? "static " : "const ") +
                         value.name + ": " + publicTypeText(value.type),
               .kind = is_static ? CompilerCompletionItemKind::Static
                                 : CompilerCompletionItemKind::Constant});
        }
        mergeCompletionOverloads(items);
        impl->completion_contexts.push_back(
            {{.path = path, .range = {.start = start, .end = end}},
             std::move(items)});
      }
    }
  }
  std::ranges::sort(impl->occurrences, [](const auto &left, const auto &right) {
    return std::tie(left.location.path, left.location.range.start.line,
                    left.location.range.start.character) <
           std::tie(right.location.path, right.location.range.start.line,
                    right.location.range.start.character);
  });
  return std::shared_ptr<const CompilerWorkspaceSymbolIndex>(
      new CompilerWorkspaceSymbolIndex(std::move(impl)));
}

std::vector<CompilerCompletionItem>
CompilerWorkspaceSymbolIndex::completion(std::string_view path,
                                     CompilerTextPosition position,
                                     std::string_view prefix) const {
  const auto normalized = normalizeCompilerInputPath(path);
  for (const auto &context : impl_->completion_contexts) {
    if (context.location.path != normalized ||
        position.line != context.location.range.start.line ||
        position.character < context.location.range.start.character ||
        position.character > context.location.range.end.character)
      continue;
    std::vector<CompilerCompletionItem> result;
    for (const auto &item : context.items)
      if (item.label.starts_with(prefix))
        result.push_back(item);
    return result;
  }
  return {};
}

std::optional<CompilerHoverResult>
CompilerWorkspaceSymbolIndex::hover(std::string_view path,
                                CompilerTextPosition position) const {
  const auto *occurrence = impl_->find(path, position);
  if (!occurrence)
    return std::nullopt;
  const auto entity = impl_->entities.find(occurrence->key);
  if (entity == impl_->entities.end())
    return std::nullopt;
  return CompilerHoverResult{entity->second.hover, occurrence->location.range};
}

std::vector<CompilerSourceLocation>
CompilerWorkspaceSymbolIndex::definition(std::string_view path,
                                     CompilerTextPosition position) const {
  const auto *occurrence = impl_->find(path, position);
  if (!occurrence)
    return {};
  const auto entity = impl_->entities.find(occurrence->key);
  return entity != impl_->entities.end() && entity->second.declaration
             ? std::vector{*entity->second.declaration}
             : std::vector<CompilerSourceLocation>{};
}

std::vector<CompilerSourceLocation>
CompilerWorkspaceSymbolIndex::references(std::string_view path,
                                     CompilerTextPosition position,
                                     bool include_declaration) const {
  const auto *selected = impl_->find(path, position);
  if (!selected)
    return {};
  std::vector<CompilerSourceLocation> result;
  for (const auto &occurrence : impl_->occurrences) {
    if (occurrence.key == selected->key &&
        (include_declaration || !occurrence.declaration))
      result.push_back(occurrence.location);
  }
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::vector<CompilerDocumentSymbol>
CompilerWorkspaceSymbolIndex::documentSymbols(std::string_view path) const {
  const auto normalized = normalizeCompilerInputPath(path);
  std::vector<CompilerDocumentSymbol> result;
  for (const auto &occurrence : impl_->occurrences) {
    if (!occurrence.declaration || occurrence.location.path != normalized)
      continue;
    const auto entity = impl_->entities.find(occurrence.key);
    if (entity == impl_->entities.end() || !entity->second.document_symbol ||
        entity->second.name.empty())
      continue;
    result.push_back({.name = entity->second.name,
                      .kind = entity->second.symbol_kind,
                      .range = occurrence.location.range,
                      .selection_range = occurrence.location.range});
  }
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::optional<CompilerRenameResult>
CompilerWorkspaceSymbolIndex::prepareRename(std::string_view path,
                                        CompilerTextPosition position) const {
  const auto *selected = impl_->find(path, position);
  if (!selected)
    return std::nullopt;
  const auto entity = impl_->entities.find(selected->key);
  if (entity == impl_->entities.end() || !entity->second.declaration ||
      entity->second.name.empty())
    return std::nullopt;
  CompilerRenameResult result{.placeholder = entity->second.name,
                          .range = selected->location.range};
  for (const auto &occurrence : impl_->occurrences)
    if (occurrence.key == selected->key)
      result.locations.push_back(occurrence.location);
  result.locations.erase(
      std::unique(result.locations.begin(), result.locations.end()),
      result.locations.end());
  return result;
}

std::optional<CompilerRenameResult>
CompilerWorkspaceSymbolIndex::rename(std::string_view path,
                                 CompilerTextPosition position,
                                 std::string_view new_name,
                                 std::string &error) const {
  error.clear();
  if (new_name.empty() ||
      (!std::isalpha(static_cast<unsigned char>(new_name.front())) &&
       new_name.front() != '_') ||
      std::ranges::any_of(new_name, [](unsigned char character) {
        return !std::isalnum(character) && character != '_';
      }) ||
      compiler::keywordKind(new_name) != compiler::TokenKind::Identifier) {
    error = "rename requires a non-keyword ASCII identifier";
    return std::nullopt;
  }
  auto result = prepareRename(path, position);
  if (!result)
    error = "the selected symbol cannot be renamed";
  return result;
}

} // namespace chtholly
