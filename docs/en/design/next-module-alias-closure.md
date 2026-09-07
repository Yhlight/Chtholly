# Next Module Alias Closure

Status: implemented and verified as a post-v1 source capability.

## Semantic Boundary

`import module as alias;` and `export import module as alias;` create a
consumer-local qualified lookup key. The alias is not a module identity,
public entity name, LLVM symbol component, or artifact fingerprint input.
Forwarding an import copies the provider's canonical public closure; it does
not create wrapper entities or expose non-public bindings.

Import collection resolves provider identity before building `ImportIR`. The
collector rejects duplicate lookup keys, duplicate providers, local-module
collisions, and ambiguous dependency providers. `ImportIRTable::verify`
rechecks lookup-key uniqueness so malformed in-memory or decoded state cannot
reach semantic name lookup.

## Artifact And Incremental Rules

Direct imports observe module presence. `export import` observes the provider
public-interface fingerprint as an export-set dependency. Renaming a local
alias changes only the consumer source and semantic fingerprints; it does not
change provider entities or provider artifacts. A provider interface change
invalidates a forwarding facade through its export-set observation.