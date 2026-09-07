# Next Conversion Target Audit

Status: complete for the frozen v1 conversion surface.

Date: 2026-08-21

## Implemented Conversion Kinds

`ConversionPlan` separates selection from mutation. `queryConversion` returns
one of these explicit kinds and a stable overload rank; `applyConversion`
performs the selected SemIR change.

| Chtholly kind | Admitted use | SemIR action |
|---|---|---|
| `Identity` | exact type | none |
| `Never` | diverging expression to any required result | none |
| `ContextualInteger` | fitting unsuffixed integer literal | retag literal |
| `ContextualIntegerToFloat` | exactly representable integer literal | emit float literal |
| `ContextualFloat` | representable float literal | retag or rebuild literal |
| `ContextualNull` | `null` with a raw-pointer target | retag null |
| `Numeric` | lossless implicit numeric conversion | `SemNumericConvert` |
| `ReferenceToValue` | transparent reference alias chain to its pointee value | explicit SemIR dereference chain/read |
| `ValueToReference` | addressable value or short-lived temporary to `const T&` | materialize (if needed) and borrow |
| `ReferenceAuthority` | reference alias chain to a capability-compatible reference | reborrow from final place |
| `PointerAuthority` | raw-pointer const/void authority narrowing | retag pointer |

The query is used consistently by contextual typing, common-type selection,
overload ranking, argument adjustment, binding initialization, assignment,
return checking, aggregate construction, and explicit casts.