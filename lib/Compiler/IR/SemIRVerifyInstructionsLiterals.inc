// Included inside SemIRVerificationContext::verifyInstructions; the shared
// read-only context locals and diagnostics remain owned by the caller.

    case SemInstKind::Invalid:
      error = "semantic IR contains an invalid instruction";
      return false;
    case SemInstKind::FunctionDecl: {
      const auto &entity = function(FunctionId(value.arg0));
      if (instruction_type != void_type_ || value.arg1 != entity.body.index) {
        error = "function declaration does not match its entity";
        return false;
      }
      break;
    }
    case SemInstKind::ConstantDecl: {
      const auto &entity = constantEntity(ConstantEntityId(value.arg0));
      if (instruction_type != void_type_ ||
          value.arg1 != entity.initializer.index) {
        error = "constant declaration does not match its entity";
        return false;
      }
      break;
    }
    case SemInstKind::ConstantRef:
    case SemInstKind::StaticRef:
      if (instruction_type !=
          constantEntity(ConstantEntityId(value.arg0)).type) {
        error = "constant or static reference type does not match its entity";
        return false;
      }
      if ((value.kind == SemInstKind::StaticRef) !=
          ((constantEntity(ConstantEntityId(value.arg0)).flags &
            SemConstantStatic) != 0)) {
        error = "constant reference kind does not match its entity";
        return false;
      }
      break;
    case SemInstKind::VoidValue:
      if (instruction_type != void_type_) {
        error = "void value has a non-void type";
        return false;
      }
      break;
    case SemInstKind::Parameter:
      if (instruction_type != local(LocalId(value.arg0)).type) {
        error = "parameter type does not match its local";
        return false;
      }
      break;
    case SemInstKind::IntegerLiteral:
      if (type(instruction_type).kind != SemTypeKind::Integer) {
        error = "integer literal has a non-integer type";
        return false;
      }
      break;
    case SemInstKind::FloatLiteral:
      if (type(instruction_type).kind != SemTypeKind::Float) {
        error = "floating-point literal has a non-floating-point type";
        return false;
      }
      break;
    case SemInstKind::BoolLiteral:
      if (instruction_type != bool_type_) {
        error = "boolean literal has a non-boolean type";
        return false;
      }
      break;
    case SemInstKind::CharLiteral:
      if (instruction_type != sem_ir_.char_type_ ||
          static_cast<std::uint64_t>(integer(IntegerId(value.arg0))) >
              0x10ffffU) {
        error = "character literal has an invalid Char value";
        return false;
      }
      break;
    case SemInstKind::StringLiteral:
      if (instruction_type != string_type_ &&
          type(instruction_type).kind != SemTypeKind::RawPointer) {
        error = "string literal has a non-string type";
        return false;
      }
      break;
    case SemInstKind::NullPointer:
      if (type(instruction_type).kind != SemTypeKind::RawPointer &&
          type(instruction_type).kind != SemTypeKind::CoroutineExecutor) {
        error = "null literal has a non-pointer type";
        return false;
      }
      break;
    case SemInstKind::ArrayLiteral: {
      const auto &array_type = type(instruction_type);
      const auto elements = instBlock(InstBlockId(value.arg0));
      if (array_type.kind != SemTypeKind::Array ||
          array_type.arg1 != elements.size()) {
        error = "array literal does not match its array type";
        return false;
      }
      for (const auto element : elements) {
        if (inst(element).type != array_type.arg0) {
          error = "array literal element has the wrong type";
          return false;
        }
      }
      break;
    }
    case SemInstKind::TupleLiteral: {
      const auto &tuple_type = type(instruction_type);
      const auto elements = instBlock(InstBlockId(value.arg0));
      if (tuple_type.kind != SemTypeKind::Tuple ||
          tuple_type.arg0 == core::AnyId::InvalidIndex ||
          typeBlock(TypeBlockId(tuple_type.arg0)).size() != elements.size()) {
        error = "tuple literal does not match its tuple type";
        return false;
      }
      const auto element_types = typeBlock(TypeBlockId(tuple_type.arg0));
      for (std::size_t element_index = 0; element_index < elements.size();
           ++element_index)
        if (TypeId(inst(elements[element_index]).type) !=
            element_types[element_index]) {
          error = "tuple literal element has the wrong type";
          return false;
        }
      break;
    }
    case SemInstKind::AggregateInit: {
      const auto &aggregate_type = type(instruction_type);
      if (aggregate_type.kind != SemTypeKind::Nominal ||
          nominalType(NominalTypeId(aggregate_type.arg0)).kind !=
              NominalKind::Struct ||
          instBlock(InstBlockId(value.arg0)).size() !=
              nominalType(NominalTypeId(aggregate_type.arg0)).fields.size()) {
        error = "aggregate initializer does not match its nominal type";
        return false;
      }
      break;
    }
    case SemInstKind::Closure: {
      const auto &environment_type = type(instruction_type);
      if (environment_type.kind != SemTypeKind::Nominal) {
        error = "closure has a non-nominal environment";
        return false;
      }
      const auto &environment =
          nominalType(NominalTypeId(environment_type.arg0));
      const auto captures = instBlock(InstBlockId(value.arg1));
      if ((environment.flags & SemNominalTypeClosureEnvironment) == 0 ||
          captures.size() != environment.fields.size()) {
        error = "closure environment does not match its captures";
        return false;
      }
      for (std::uint32_t capture = 0; capture < captures.size(); ++capture) {
        if (TypeId(inst(captures[capture]).type) !=
            nominalFieldType(instruction_type, capture)) {
          error = "closure capture has the wrong environment field type";
          return false;
        }
      }
      const auto target = FunctionRefId(value.arg0);
      if (!target.hasValue() || target.index >= functionRefCount()) {
        error = "closure has an invalid hidden target";
        return false;
      }
      const auto &target_type = type(functionRef(target).local_type);
      const auto parameters = target_type.kind == SemTypeKind::Function
                                  ? typeBlock(TypeBlockId(target_type.arg0))
                                  : std::span<const TypeId>{};
      if (parameters.empty() ||
          type(parameters.front()).kind != SemTypeKind::Reference ||
          referencePointee(parameters.front()) != instruction_type) {
        error = "closure hidden target does not match its environment";
        return false;
      }
      const auto *info = tryGetCallableEnvironment(instruction_type);
      const auto capability = referenceMutability(parameters.front()) ==
                                      SemReferenceMutability::Mutable
                                  ? SemCallableEnvironmentCapability::Mutable
                                  : SemCallableEnvironmentCapability::ReadOnly;
      if (!info || info->kind != SemCallableEnvironmentKind::Closure ||
          info->target != target || info->capability != capability ||
          !info->identity.hasValue()) {
        error = "closure callable-environment facts are incomplete";
        return false;
      }
      break;
    }
    case SemInstKind::BoundMethod: {
      const auto &environment_type = type(instruction_type);
      if (environment_type.kind != SemTypeKind::Nominal) {
        error = "bound method has an invalid environment or target";
        return false;
      }
      const auto &environment =
          nominalType(NominalTypeId(environment_type.arg0));
      if ((environment.flags & SemNominalTypeBoundMethodEnvironment) == 0 ||
          environment.fields.size() != 1 ||
          first_inst_type() != nominalFieldType(instruction_type, 0)) {
        error = "bound method environment does not match its receiver";
        return false;
      }
      const auto target = FunctionRefId(value.arg1);
      if (!target.hasValue() || target.index >= functionRefCount()) {
        error = "bound method has an invalid target reference";
        return false;
      }
      const auto &target_type = type(functionRef(target).local_type);
      if (target_type.kind != SemTypeKind::Function ||
          typeBlock(TypeBlockId(target_type.arg0)).empty()) {
        error = "bound method target does not match its receiver";
        return false;
      }
      const auto field_type = nominalFieldType(instruction_type, 0);
      auto target_receiver = typeBlock(TypeBlockId(target_type.arg0)).front();
      if (target_receiver != field_type &&
          (type(target_receiver).kind != SemTypeKind::Reference ||
           referencePointee(target_receiver) != field_type)) {
        error = "bound method target does not match its receiver";
        return false;
      }
      const auto *info = tryGetCallableEnvironment(instruction_type);
      const auto capability =
          type(target_receiver).kind == SemTypeKind::Reference
              ? referenceMutability(target_receiver) ==
                        SemReferenceMutability::Mutable
                    ? SemCallableEnvironmentCapability::Mutable
                    : SemCallableEnvironmentCapability::ReadOnly
              : SemCallableEnvironmentCapability::Consuming;
      if (!info || info->kind != SemCallableEnvironmentKind::BoundMethod ||
          info->formation_target != target || info->capability != capability ||
          !info->identity.hasValue()) {
        error = "bound method callable-environment facts are incomplete";
        return false;
      }
      break;
    }
    case SemInstKind::UnionInit: {
      const auto &aggregate_type = type(instruction_type);
      const auto member = integer(IntegerId(value.arg1));
      if (aggregate_type.kind != SemTypeKind::Nominal || member < 0) {
        error = "union initializer has invalid semantic facts";
        return false;
      }
      const auto &nominal = nominalType(NominalTypeId(aggregate_type.arg0));
      if (nominal.kind != NominalKind::Union ||
          static_cast<std::size_t>(member) >= nominal.fields.size() ||
          first_inst_type() !=
              nominalFieldType(instruction_type,
                               static_cast<std::uint32_t>(member))) {
        error = "union initializer does not match its active member";
        return false;
      }
      break;
    }
    case SemInstKind::EnumInit: {
      const auto &enum_type = type(instruction_type);
      const auto variant_index = integer(IntegerId(value.arg1));
      if (enum_type.kind != SemTypeKind::Nominal || variant_index < 0) {
        error = "enum initializer has invalid semantic facts";
        return false;
      }
      const auto &nominal = nominalType(NominalTypeId(enum_type.arg0));
      if (nominal.kind != NominalKind::Enum ||
          static_cast<std::size_t>(variant_index) >= nominal.variants.size()) {
        error = "enum initializer names an invalid variant";
        return false;
      }
      const auto payload = instBlock(InstBlockId(value.arg0));
      const auto &variant = nominal.variants[variant_index];
      if (payload.size() != variant.fields.size()) {
        error = "enum initializer has an invalid payload arity";
        return false;
      }
      for (std::uint32_t field = 0; field < payload.size(); ++field)
        if (TypeId(inst(payload[field]).type) !=
            enumPayloadFieldType(instruction_type,
                                 static_cast<std::uint32_t>(variant_index),
                                 field)) {
          error = "enum initializer payload type does not match its variant";
          return false;
        }
      break;
    }
    case SemInstKind::EnumTag: {
      const auto &enum_type = type(first_inst_type());
      const auto &tag_type = type(instruction_type);
      const auto *nominal = enum_type.kind == SemTypeKind::Nominal
                                ? &nominalType(NominalTypeId(enum_type.arg0))
                                : nullptr;
      if (enum_type.kind != SemTypeKind::Nominal || !nominal ||
          nominal->kind != NominalKind::Enum ||
          tag_type.kind != SemTypeKind::Integer || tag_type.arg0 != 32 ||
          tag_type.arg1 != (nominal->is_value_enum ? 1U : 0U)) {
        error = "enum tag read has inconsistent semantic facts";
        return false;
      }
      break;
    }
    case SemInstKind::EnumPayloadAccess: {
      const auto enum_type = first_inst_type();
      const auto encoded =
          static_cast<std::uint64_t>(integer(IntegerId(value.arg1)));
      const auto variant = static_cast<std::uint32_t>(encoded >> 32U);
      const auto field = static_cast<std::uint32_t>(encoded);
      if (type(enum_type).kind != SemTypeKind::Nominal ||
          instruction_type != enumPayloadFieldType(enum_type, variant, field)) {
        error = "enum payload projection has inconsistent semantic facts";
        return false;
      }
      break;
    }
    case SemInstKind::NameRef:
      if (instruction_type != local(LocalId(value.arg0)).type) {
        error = "name reference type does not match its local";
        return false;
      }
      break;
    case SemInstKind::BorrowLocal: {
      const auto local_type = local(LocalId(value.arg0)).type;
      if (type(instruction_type).kind != SemTypeKind::Reference ||
          referencePointee(instruction_type) != local_type) {
        error = "borrow instruction does not match its local";
        return false;
      }
      break;
    }
    case SemInstKind::Dereference: {
      const auto operand_type = first_inst_type();
      const auto operand_kind = type(operand_type).kind;
      const auto pointee = operand_kind == SemTypeKind::Reference
                               ? referencePointee(operand_type)
                           : operand_kind == SemTypeKind::RawPointer
                               ? rawPointerPointee(operand_type)
                               : TypeId::invalid();
      if (!pointee.hasValue() || pointee != instruction_type ||
          instruction_type == void_type_) {
        error = "dereference instruction has inconsistent semantic facts";
        return false;
      }
      break;
    }
    case SemInstKind::CarrierView: {
      const auto input = first_inst_type();
      if (type(input).kind != SemTypeKind::Reference ||
          type(instruction_type).kind != SemTypeKind::Reference ||
          referenceMutability(input) != referenceMutability(instruction_type) ||
          integer(IntegerId(value.arg1)) < 0) {
        error = "carrier view has inconsistent reference facts";
        return false;
      }
      break;
    }
    case SemInstKind::BorrowPlace:
      if (type(instruction_type).kind != SemTypeKind::Reference ||
          referencePointee(instruction_type) != first_inst_type() ||
          contains_raw_pointer_dereference(contains_raw_pointer_dereference,
                                           InstId(value.arg0))) {
        error = "borrow-place instruction has inconsistent semantic facts";
        return false;
      }
      break;
    case SemInstKind::Move:
    case SemInstKind::Copy:
      if (instruction_type != first_inst_type()) {
        error = "lifecycle value instruction does not preserve its place type";
        return false;
      }
      if (contains_raw_pointer_dereference(contains_raw_pointer_dereference,
                                           InstId(value.arg0))) {
        error = "raw-pointer pointees do not support lifecycle transfer";
        return false;
      }
      break;
    case SemInstKind::MemberAccess: {
      const auto slice = type(first_inst_type()).kind == SemTypeKind::Slice;
      const auto wide_length =
          type(instruction_type).kind == SemTypeKind::Integer &&
          type(instruction_type).arg0 == 64 && type(instruction_type).arg1 == 0;
      if ((first_inst_type() != string_type_ && !slice) ||
          (slice ? instruction_type != i32_type_ && !wide_length
                 : instruction_type != i32_type_) ||
          identifier(name(NameId(value.arg1)).text) != "len") {
        error = "member access has inconsistent semantic facts";
        return false;
      }
      break;
    }
    case SemInstKind::StructFieldAccess: {
      const auto &base = type(first_inst_type());
      const auto field_index = integer(IntegerId(value.arg1));
      const bool valid_tuple =
          base.kind == SemTypeKind::Tuple && field_index >= 0 &&
          static_cast<std::size_t>(field_index) <
              typeBlock(TypeBlockId(base.arg0)).size() &&
          instruction_type ==
              tupleElementType(first_inst_type(),
                               static_cast<std::uint32_t>(field_index));
      const bool valid_nominal =
          base.kind == SemTypeKind::Nominal && field_index >= 0 &&
          static_cast<std::size_t>(field_index) <
              nominalType(NominalTypeId(base.arg0)).fields.size() &&
          nominalType(NominalTypeId(base.arg0)).kind == NominalKind::Struct;
      if (!valid_tuple && !valid_nominal) {
        error = "struct field access has inconsistent semantic facts";
        return false;
      }
      break;
    }
    case SemInstKind::UnionFieldAccess: {
      const auto base_type = first_inst_type();
      const auto &base = type(base_type);
      auto field_index = integer(IntegerId(value.arg1));
      if (field_index == std::numeric_limits<std::int64_t>::min()) {
        error = "union field access has invalid semantic facts";
        return false;
      }
      if (field_index < 0)
        field_index = -field_index - 1;
      if (base.kind != SemTypeKind::Nominal || field_index < 0) {
        error = "union field access has invalid semantic facts";
        return false;
      }
      const auto &nominal = nominalType(NominalTypeId(base.arg0));
      if (nominal.kind != NominalKind::Union ||
          static_cast<std::size_t>(field_index) >= nominal.fields.size() ||
          instruction_type !=
              nominalFieldType(base_type,
                               static_cast<std::uint32_t>(field_index))) {
        error = "union field access does not match its member";
        return false;
      }
      break;
    }
    case SemInstKind::Index: {
      const auto &base_type = type(first_inst_type());
      const auto index_type = TypeId(inst(InstId(value.arg1)).type);
      const auto valid_index =
          index_type == i32_type_ ||
          (type(index_type).kind == SemTypeKind::Integer &&
           type(index_type).arg0 == 64 && type(index_type).arg1 == 0);
      if ((base_type.kind != SemTypeKind::Array &&
           base_type.kind != SemTypeKind::Slice) ||
          !valid_index || instruction_type.index != base_type.arg0) {
        error = "index instruction has inconsistent semantic facts";
        return false;
      }
      break;
    }
    case SemInstKind::Slice: {
      const auto operands = instBlock(InstBlockId(value.arg0));
      const auto valid_index = [&](InstId id) {
        const auto index_type = TypeId(inst(id).type);
        return index_type == i32_type_ ||
               (type(index_type).kind == SemTypeKind::Integer &&
                type(index_type).arg0 == 64 && type(index_type).arg1 == 0);
      };
      if (operands.size() != 3 ||
          type(instruction_type).kind != SemTypeKind::Slice ||
          type(TypeId(inst(operands[0]).type)).kind != SemTypeKind::Reference ||
          (type(referencePointee(TypeId(inst(operands[0]).type))).kind !=
               SemTypeKind::Array &&
           type(referencePointee(TypeId(inst(operands[0]).type))).kind !=
               SemTypeKind::Slice) ||
          !valid_index(operands[1]) || !valid_index(operands[2]) ||
          inst(operands[1]).type != inst(operands[2]).type) {
        error = "slice construction has inconsistent semantic facts";
        return false;
      }
      break;
    }
    case SemInstKind::BuiltinUnary: {
      const auto raw = integer(IntegerId(value.arg1));
      const auto operation = static_cast<BuiltinOperatorKind>(raw);
      const auto kind = type(instruction_type).kind;
      if (raw < 0 || operation >= BuiltinOperatorKind::Count ||
          !isBuiltinUnaryOperator(operation) ||
          first_inst_type() != instruction_type ||
          (operation == BuiltinOperatorKind::BitNot
               ? kind != SemTypeKind::Integer
               : kind != SemTypeKind::Integer && kind != SemTypeKind::Float)) {
        error = "builtin unary instruction has inconsistent semantic facts";
        return false;
      }
      break;
    }
    case SemInstKind::BuiltinBinary: {
      const auto operands = instBlock(InstBlockId(value.arg0));
      const auto raw = integer(IntegerId(value.arg1));
      const auto operation = static_cast<BuiltinOperatorKind>(raw);
      if (raw < 0 || operation >= BuiltinOperatorKind::Count ||
          isBuiltinUnaryOperator(operation) || operands.size() != 2) {
        error = "builtin binary instruction has invalid operands";
        return false;
      }
      const auto left = TypeId(inst(operands[0]).type);
      const auto right = TypeId(inst(operands[1]).type);
      const auto left_kind = type(left).kind;
      const auto shift = isBuiltinShiftOperator(operation);
      const auto comparison = isBuiltinComparisonOperator(operation);
      const auto equality = isBuiltinEqualityOperator(operation);
      const auto integer_only = isBuiltinIntegerOnlyOperator(operation);
      const auto compatible = shift
                                  ? left_kind == SemTypeKind::Integer &&
                                        type(right).kind == SemTypeKind::Integer
                                  : left == right;
      const auto domain =
          integer_only ? left_kind == SemTypeKind::Integer
          : comparison
              ? left_kind == SemTypeKind::Integer ||
                    left_kind == SemTypeKind::Float ||
                    (equality &&
                     (left_kind == SemTypeKind::Bool ||
                      left_kind == SemTypeKind::RawPointer ||
                      left_kind == SemTypeKind::CFunctionPointer ||
                      left_kind == SemTypeKind::CVariadicFunctionPointer ||
                      left_kind == SemTypeKind::Char))
              : left_kind == SemTypeKind::Integer ||
                    left_kind == SemTypeKind::Float;
      const auto result_ok =
          operation == BuiltinOperatorKind::ThreeWay
              ? type(instruction_type).kind == SemTypeKind::Nominal
          : comparison ? instruction_type == bool_type_
                       : instruction_type == left;
      if (!compatible || !domain || !result_ok) {
        error = "builtin binary instruction has inconsistent semantic facts";
        return false;
      }
      if (operation == BuiltinOperatorKind::ThreeWay) {
        const auto &nominal =
            nominalType(NominalTypeId(type(instruction_type).arg0));
        const auto *entity = importIRs().tryGetEntity(nominal.canonical_entity);
        constexpr std::array<std::string_view, 4> names{"Less", "Equal",
                                                        "Greater", "Unordered"};
        const auto valid_variant = [&](std::string_view expected) {
          return std::ranges::any_of(
              nominal.variants, [&](const auto &variant) {
                return identifier(name(variant.name).text) == expected &&
                       variant.shape == SemEnumPayloadShape::Unit &&
                       variant.fields.empty();
              });
        };
        if (!entity || identifier(entity->package_name) != "std" ||
            identifier(entity->module_name) != "std::compare" ||
            identifier(entity->name) != "Ordering" ||
            nominal.kind != NominalKind::Enum ||
            nominal.variants.size() != names.size() ||
            !std::ranges::all_of(names, valid_variant)) {
          error = "three-way comparison does not use canonical Ordering";
          return false;
        }
      }
      break;
    }
    case SemInstKind::Add:
      if (first_inst_type() != instruction_type ||
          TypeId(inst(InstId(value.arg1)).type) != instruction_type ||
          (type(instruction_type).kind != SemTypeKind::Integer &&
           type(instruction_type).kind != SemTypeKind::Float)) {
        error = "add instruction has inconsistent semantic facts";
        return false;
      }
      break;
    case SemInstKind::NumericConvert: {
      const auto source = type(first_inst_type()).kind;
      const auto target = type(instruction_type).kind;
      const auto source_numeric = source == SemTypeKind::Integer ||
                                  source == SemTypeKind::Float ||
                                  source == SemTypeKind::Char;
      const auto target_numeric = target == SemTypeKind::Integer ||
                                  target == SemTypeKind::Float ||
                                  target == SemTypeKind::Char;
      if (!source_numeric || !target_numeric) {
        error = "numeric conversion has non-numeric types";
        return false;
      }
      break;
    }
    case SemInstKind::CheckedNumericCast: {
      const auto source = type(first_inst_type()).kind;
      const auto &result = type(instruction_type);
      const auto result_arguments = result.kind == SemTypeKind::Nominal
                                        ? typeBlock(TypeBlockId(result.arg1))
                                        : std::span<const TypeId>{};
      if ((source != SemTypeKind::Integer && source != SemTypeKind::Float &&
           source != SemTypeKind::Char) ||
          result.kind != SemTypeKind::Nominal || result_arguments.size() != 2) {
        error = "checked numeric cast has invalid types";
        return false;
      }
      const auto target = result_arguments.front();
      if (type(target).kind != SemTypeKind::Integer &&
          type(target).kind != SemTypeKind::Float &&
          type(target).kind != SemTypeKind::Char) {
        error = "checked numeric cast has a non-numeric target";
        return false;
      }
      const auto canonical_is = [&](const SemNominalType &nominal,
                                    std::string_view module,
                                    std::string_view name) {
        const auto *entity = importIRs().tryGetEntity(nominal.canonical_entity);
        return entity && identifier(entity->package_name) == "std" &&
               identifier(entity->module_name) == module &&
               identifier(entity->name) == name;
      };
      const auto &result_nominal = nominalType(NominalTypeId(result.arg0));
      const auto &error_type = type(result_arguments.back());
      if (result_nominal.kind != NominalKind::Enum ||
          result_nominal.variants.size() != 2 ||
          !canonical_is(result_nominal, "std::result", "Result") ||
          error_type.kind != SemTypeKind::Nominal) {
        error = "checked numeric cast does not use canonical Result";
        return false;
      }
      const auto &error_nominal = nominalType(NominalTypeId(error_type.arg0));
      if (error_nominal.kind != NominalKind::Enum ||
          error_nominal.variants.size() != 3 ||
          !canonical_is(error_nominal, "std::convert", "CastError")) {
        error = "checked numeric cast does not use canonical CastError";
        return false;
      }
      const auto find_variant =
          [&](const SemNominalType &nominal, std::string_view name,
              SemEnumPayloadShape shape, std::size_t fields) {
            for (std::uint32_t index = 0; index < nominal.variants.size();
                 ++index) {
              const auto &variant = nominal.variants[index];
              if (identifier(this->name(variant.name).text) == name &&
                  variant.shape == shape && variant.fields.size() == fields)
                return index;
            }
            return core::AnyId::InvalidIndex;
          };
      const auto ok_variant =
          find_variant(result_nominal, "Ok", SemEnumPayloadShape::Tuple, 1);
      const auto err_variant =
          find_variant(result_nominal, "Err", SemEnumPayloadShape::Tuple, 1);
      if (ok_variant == core::AnyId::InvalidIndex ||
          err_variant == core::AnyId::InvalidIndex ||
          enumPayloadFieldType(instruction_type, ok_variant, 0) != target ||
          enumPayloadFieldType(instruction_type, err_variant, 0) !=
              result_arguments.back() ||
          find_variant(error_nominal, "Inexact", SemEnumPayloadShape::Unit,
                       0) == core::AnyId::InvalidIndex ||
          find_variant(error_nominal, "OutOfRange", SemEnumPayloadShape::Unit,
                       0) == core::AnyId::InvalidIndex ||
          find_variant(error_nominal, "NonFinite", SemEnumPayloadShape::Unit,
                       0) == core::AnyId::InvalidIndex) {
        error = "checked numeric cast has invalid Result or CastError variants";
        return false;
      }
      break;
    }
    case SemInstKind::Equal:
      if (first_inst_type().index != inst(InstId(value.arg1)).type ||
          instruction_type != bool_type_) {
        error = "equality instruction has inconsistent semantic facts";
        return false;
      }
      break;
    case SemInstKind::LogicalNot:
      if (first_inst_type() != bool_type_ || instruction_type != bool_type_) {
        error = "logical not has inconsistent semantic facts";
        return false;
      }
      break;
    case SemInstKind::LogicalAnd:
    case SemInstKind::LogicalOr:
      if (first_inst_type() != bool_type_ ||
          TypeId(inst(InstId(value.arg1)).type) != bool_type_ ||
          instruction_type != bool_type_) {
        error = "logical binary instruction has inconsistent semantic facts";
        return false;
      }
      break;
    case SemInstKind::MaterializeTemporary:
      if (instruction_type != local(LocalId(value.arg0)).type ||
          instruction_type != TypeId(inst(InstId(value.arg1)).type) ||
          (local(LocalId(value.arg0)).flags & SemLocalTemporary) == 0) {
        error = "temporary materialization has inconsistent storage facts";
        return false;
      }
      break;
    case SemInstKind::EndFullExpression:
      if (instruction_type != void_type_) {
        error = "full-expression boundary must have void type";
        return false;
      }
      for (const auto temporary : instBlock(InstBlockId(value.arg0))) {
        if (inst(temporary).kind != SemInstKind::MaterializeTemporary) {
          error = "full-expression boundary contains a non-temporary";
          return false;
        }
      }
      break;
    case SemInstKind::ExtendTemporary:
      if (instruction_type != void_type_ ||
          inst(InstId(value.arg1)).kind != SemInstKind::MaterializeTemporary ||
          (local(LocalId(value.arg0)).flags & SemLocalTemporary) != 0 ||
          type(local(LocalId(value.arg0)).type).kind !=
              SemTypeKind::Reference ||
          referenceMutability(local(LocalId(value.arg0)).type) !=
              SemReferenceMutability::ReadOnly) {
        error = "temporary lifetime extension has inconsistent binding facts";
        return false;
      }
      break;
    case SemInstKind::BindName:
      if (instruction_type != void_type_ ||
          local(LocalId(value.arg0)).type !=
              TypeId(inst(InstId(value.arg1)).type)) {
        error = "binding instruction " + std::to_string(index) +
                " has inconsistent semantic facts: local type " +
                std::to_string(local(LocalId(value.arg0)).type.index) +
                ", value type " + std::to_string(inst(InstId(value.arg1)).type);
        return false;
      }
      break;
    case SemInstKind::DeclareUninitialized:
      if (instruction_type != void_type_ ||
          (local(LocalId(value.arg0)).flags & SemLocalUninitialized) == 0) {
        error = "uninitialized declaration has inconsistent semantic facts";
        return false;
      }
      break;
    case SemInstKind::OutPlace: {
      const auto local_id = LocalId(value.arg0);
      const auto reference = type(instruction_type);
      if (reference.kind != SemTypeKind::Reference ||
          referenceMutability(instruction_type) !=
              SemReferenceMutability::Mutable ||
          referencePointee(instruction_type) != local(local_id).type ||
          (local(local_id).flags & SemLocalUninitialized) == 0) {
        error = "retired place projection has inconsistent storage facts";
        return false;
      }
      break;
    }
    case SemInstKind::InitializePlace: {
      const auto local_id = LocalId(value.arg0);
      const auto reference = type(instruction_type);
      if (reference.kind != SemTypeKind::Reference ||
          referenceMutability(instruction_type) !=
              SemReferenceMutability::Mutable ||
          referencePointee(instruction_type) != local(local_id).type ||
          (local(local_id).flags & SemLocalUninitialized) == 0) {
        error = "initialize place has inconsistent storage facts";
        return false;
      }
      break;
    }
    case SemInstKind::Assign:
      if (instruction_type != void_type_ ||
          first_inst_type().index != inst(InstId(value.arg1)).type) {
        error = "assignment instruction has inconsistent semantic facts";
        return false;
      }
      break;
