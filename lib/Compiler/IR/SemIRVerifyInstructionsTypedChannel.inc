    case SemInstKind::TypedChannelSendPrepare:
    case SemInstKind::TypedChannelSendCommit: {
      const auto channel_type = type(first_inst_type()).kind ==
                                        SemTypeKind::Reference
                                    ? referencePointee(first_inst_type())
                                    : first_inst_type();
      if (instruction_type != void_type_ ||
          (type(channel_type).kind != SemTypeKind::Nominal &&
           type(channel_type).kind != SemTypeKind::RawPointer) ||
          !InstId(value.arg1).hasValue() ||
          (value.kind == SemInstKind::TypedChannelSendCommit &&
           type(TypeId(inst(InstId(value.arg1)).type)).kind ==
               SemTypeKind::Reference) ||
          type(TypeId(inst(InstId(value.arg1)).type)).kind ==
              SemTypeKind::Slice ||
          type(TypeId(inst(InstId(value.arg1)).type)).kind ==
              SemTypeKind::RawPointer) {
        error = "typed channel send transition has an invalid shape";
        return false;
      }
      break;
    }
    case SemInstKind::TypedChannelSendCancel:
    case SemInstKind::TypedChannelReceiveAcquire:
    case SemInstKind::TypedChannelReceiveCancel:
    case SemInstKind::TypedChannelClose: {
      const auto channel_type = type(first_inst_type()).kind ==
                                        SemTypeKind::Reference
                                    ? referencePointee(first_inst_type())
                                    : first_inst_type();
      if (instruction_type != void_type_ ||
          (type(channel_type).kind != SemTypeKind::Nominal &&
           type(channel_type).kind != SemTypeKind::RawPointer)) {
        error = "typed channel transition has an invalid channel operand";
        return false;
      }
      break;
    }
    case SemInstKind::TypedChannelReceiveCommit: {
      const auto channel_type = type(first_inst_type()).kind ==
                                        SemTypeKind::Reference
                                    ? referencePointee(first_inst_type())
                                    : first_inst_type();
      if (instruction_type != void_type_ ||
          (type(channel_type).kind != SemTypeKind::Nominal &&
           type(channel_type).kind != SemTypeKind::RawPointer) ||
          !InstId(value.arg1).hasValue()) {
        error = "typed channel receive commit has an invalid shape";
        return false;
      }
      break;
    }
