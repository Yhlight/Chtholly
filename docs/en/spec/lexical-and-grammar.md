# Chtholly Lexical And Grammar Specification

Status: normative for Chtholly v1. The machine-readable token admission and
operator catalog is `include/chtholly/Compiler/Grammar.def`; the frontend and the v1
surface audit consume the same records.

## Source Text And Locations

A source file is a sequence of well-formed UTF-8 bytes no larger than the
32-bit compact source-offset limit. A byte-order mark is not admitted. Invalid
UTF-8 is diagnosed before parsing. Source offsets and diagnostic columns count
bytes, lines and columns are one-based, and LF begins a new line. CRLF is one
logical line ending; a lone CR is whitespace but does not create a second line.

V1 identifiers deliberately use the stable ASCII grammar below. Unicode text
is admitted inside string, C string, character, and comment contents, but
Unicode identifiers, normalization, and confusable handling are post-v1.

```ebnf
identifier-start    = ASCII-letter | "_" ;
identifier-continue = identifier-start | ASCII-digit ;
identifier          = identifier-start, { identifier-continue } ;
ASCII-letter        = "A" ... "Z" | "a" ... "z" ;
ASCII-digit         = "0" ... "9" ;
```

## Trivia And Comments

ASCII space, horizontal tab, vertical tab, form feed, LF, and CR are trivia.
`//` consumes through the next LF or end of file. `/*` consumes through the
first following `*/`; block comments do not nest. Unterminated block comments
are errors. Comments never create tokens.

## Literals

```ebnf
digit-sequence = digit, { [ "_" ], digit } ;
integer        = decimal | "0b", binary-digits | "0o", octal-digits
               | "0x", hexadecimal-digits ;
decimal-float  = decimal-digits, ".", decimal-digits, [ exponent ]
               | decimal-digits, exponent ;
exponent       = ( "e" | "E" ), [ "+" | "-" ], decimal-digits ;
integer-suffix = "i8" | "i16" | "i32" | "i64" | "isize"
               | "u8" | "u16" | "u32" | "u64" | "usize" ;
float-suffix   = "f32" | "f64" ;
```

Separators occur only between digits. Non-decimal floating literals are not
admitted. Both sides of a decimal point contain digits: `.5` and `1.` are not
floating literals. After postfix `.`, a digit sequence is a tuple projection,
so compact spellings such as `value.0.len` do not become floating tokens.

String literals use `"..."`, C strings use `c"..."`, and character literals use
`'...'`. Their source contents are UTF-8. The admitted escapes are `\\`, `\"`,
`\'`, `\0`, `\n`, `\r`, and `\t`. Unknown escapes and unescaped line endings
are errors. A character literal decodes to exactly one Unicode scalar. A C
string produces its decoded UTF-8 byte sequence followed by a terminating zero;
an explicit embedded `\0` remains part of that sequence.

## Tokens And Admission

Fixed punctuation uses maximal munch. In particular `...` precedes `..` and
`.`, `<<=` precedes `<<` and `<`, `>>=` precedes `>>` and `>`, and `<=>`
precedes its prefixes. Keywords occupy one contiguous token interval so lookup
does not depend on locale or hash iteration order.

`TokenKind.def` is the complete scanner inventory. A token marked reserved in
`Grammar.def` is unavailable to the v1 parser even though the lexer recognizes
its spelling. `operator` is reserved for future design and admits no v1
production. Reserved recognition is not compatibility syntax.

## Expression Precedence

All admitted v1 binary operators associate left-to-right. Groups are listed
from lowest to highest precedence:

| Group | Operators |
| --- | --- |
| logical-or | `||` |
| logical-and | `&&` |
| bitwise-or | `|` |
| bitwise-xor | `^` |
| bitwise-and | `&` |
| equality | `== !=` |
| relational | `< <= > >=` |
| three-way comparison | `<=>` |
| shift | `<< >>` |
| additive | `+ -` |
| multiplicative | `* / %` |

Postfix calls, indexing, slicing, member access, and tuple projection bind more
tightly than prefix expressions. Prefix expressions bind more tightly than the
binary table. Casts are parsed after the prefix/postfix operand and before the
binary loop. Placement `expression in place` is admitted only at top-level
expression precedence. Assignment remains a statement form rather than an
expression operator.

## Grammar And Recovery Contract

Normative source productions are defined by this document and the domain
specifications listed in `docs/spec/README.md`. Every admitted
terminal must exist in `TokenKind.def`; every completed production maps to a
typed `NodeKind` or a documented contextual desugaring. The v1 surface audit
checks those identities and the shared precedence catalog.

Recovery inserts or consumes tokens only at declaration, statement, delimiter,
and comma boundaries. Recovered subtrees carry `HasError`; error nodes have no
semantic meaning, and SemIR construction may not treat them as valid source.
The parse-tree verifier remains mandatory after recovery.
