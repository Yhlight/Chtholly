# Chtholly Names And Scopes

Status: normative for Chtholly v1.

## Namespace Model

Every declaration belongs to exactly one of these namespaces:

- the value namespace contains local bindings, parameters, module constants,
  statics, and function overload sets;
- the type namespace contains nominal types;
- the module namespace contains imported module paths;
- the generic namespace contains the type parameters of the current generic
  declaration;
- the member namespace belongs to one nominal owner and contains fields and
  inherent member overload sets;
- an overload set is one value or member binding containing callable
  candidates with a common source name; and
- the label namespace is reserved for post-v1 structured loop control and has
  no v1 declaration or lookup spelling.

Namespaces are distinct. The same identifier may name a type and a value, or a
generic parameter and a module declaration. Two declarations in the same
scope and namespace conflict unless the declarations form one valid overload
set or one permitted forward-declaration/definition pair.

An imported module is found only through the module namespace. Importing a
module does not inject its declarations into any unqualified namespace.

## Scope And Declaration Point

The package module scope is established before definitions are checked.
Nominal declarations, module constants and statics, and callable declaration
shells therefore support forward reference subject to completion and cycle
rules. Declaration order does not change which module entity an identifier
denotes.

A function signature scope contains its generic parameters. Generic parameters
are visible throughout parameter types, the return type, default arguments,
and the body. Function parameters enter one value scope at function-body entry.
The function body is a nested lexical scope.

A local binding enters its value scope after its initializer has been checked.
An initializer using the same identifier therefore finds an outer binding, if
one exists, and never observes its own uninitialized declaration.

Each block introduces a lexical value scope. An `if` arm, loop body, value
block, and `task scope` body use their block scope. A `for` statement also
introduces an enclosing loop scope: its initializer bindings are visible in
the condition, step, and body and cease to exist after the loop. A `while` or
`do while` condition does not introduce bindings into its body.

Bindings introduced by an enum pattern are visible only while checking and
executing that switch arm. Duplicate pattern targets and duplicate binding
names are errors.

Default arguments are checked in the function signature scope. They may use
module entities, types, and the function's generic parameters, but cannot use
formal parameters, function locals, or a caller's scope. Their resolved
constant value and referenced public identities are part of the callable
artifact.

## Shadowing And Duplicates

A declaration in an inner lexical value scope may shadow an outer value
binding, including a parameter or module value. Lookup stops at the first
scope containing the requested namespace entry. A non-callable local that
shadows a module overload set consequently prevents an unqualified call from
selecting that overload set.

Redeclaring a name in the same lexical scope and namespace is an error.
Module constants, statics, and overload sets share the value namespace.
Nominal types use the separate type namespace. Generic parameter names must be
unique within their generic declaration.

Functions with the same value or member name form an overload set only when
their source parameter type patterns differ. Return type, parameter name,
default argument, visibility, safety, or execution kind alone cannot create an
overload. A matching forward declaration and definition denote one entity;
two definitions or incompatible redeclarations are duplicates.

Fields and inherent methods occupy their owner's member namespace. A field and
method cannot share a member name. Valid method overloads share one member
overload set. Members of unrelated owners never conflict.

## Visibility Imports And Tooling

Visibility is checked after lookup selects an entity. Private declarations are
available within their defining module but are absent from an imported public
lookup surface. Re-export preserves the canonical producer and does not create
a new declaration identity.

Semantic checking records every declaration and reference against its resolved
local or canonical public entity. Hover, definition, references, completion,
incremental dependency observations, and artifact-only lookup consume those
resolved identities. Tools must not reconstruct lookup from identifier text or
source order.

Lexical lookup scope and cleanup scope are independent compiler concepts.
Entering, suspending, or restoring a name scope does not itself register or
execute cleanup, and a cleanup edge does not change name visibility.
