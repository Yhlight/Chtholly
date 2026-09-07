#!/usr/bin/env python3
import argparse
import re
import sys
import tomllib
from pathlib import Path


MANIFEST = "support/chtholly-v1.toml"
SCHEMA_V2 = "chtholly-language-surface-v2"
SCHEMA_V3 = "chtholly-language-surface-v3"
GENERATED = "docs/spec/v1-surface.generated.md"
ID = re.compile(r"[a-z][a-z0-9-]*\Z")
TOKEN_RECORD = re.compile(
    r'CHTHOLLY_COMPILER_TOKEN\(([A-Za-z0-9_]+),\s*"([^"]*)"\)')
RESERVED_RECORD = re.compile(
    r"CHTHOLLY_COMPILER_RESERVED_TOKEN\(([A-Za-z0-9_]+)\)")
BINARY_RECORD = re.compile(
    r"CHTHOLLY_COMPILER_BINARY_OPERATOR\("
    r"([A-Za-z0-9_]+),\s*([A-Za-z0-9_]+),\s*([0-9]+),\s*"
    r"(Left|Right|None)\)")
EVIDENCE_CATEGORIES = (
    "positive", "negative", "version", "cross_package", "llvm_execution")
TEST_NAME_RECORD = re.compile(r'^\s*name\s*=\s*"([^"]+)"', re.MULTILINE)


class SurfaceError(Exception):
    pass


def semantic_sources(root: Path) -> str:
    paths = [root / "lib/Compiler/Semantics/Semantics.cpp",
             *sorted((root / "lib/Compiler/Semantics").glob(
                 "SemanticContext*.cpp")),
             *sorted((root / "lib/Compiler/Semantics").glob(
                 "SemanticContext*.inc"))]
    return "\n".join(path.read_text(encoding="utf-8") for path in paths)


def validate_grammar(root: Path) -> None:
    token_text = (root / "include/chtholly/Compiler/TokenKind.def").read_text(
        encoding="utf-8")
    records = TOKEN_RECORD.findall(token_text)
    if not records:
        raise SurfaceError("TokenKind.def has no machine-readable token records")
    names = [name for name, _ in records]
    spellings = [spelling for _, spelling in records if spelling]
    if len(names) != len(set(names)):
        raise SurfaceError("TokenKind.def contains duplicate token names")
    if len(spellings) != len(set(spellings)):
        raise SurfaceError("TokenKind.def contains duplicate fixed spellings")
    keyword_names = names[names.index("KwAs"):names.index("KwWhile") + 1]
    if any(not name.startswith("Kw") for name in keyword_names):
        raise SurfaceError("keyword tokens must remain one contiguous interval")

    grammar_text = (root / "include/chtholly/Compiler/Grammar.def").read_text(
        encoding="utf-8")
    reserved = [name for name in RESERVED_RECORD.findall(grammar_text)
                if name != "Name"]
    binary = BINARY_RECORD.findall(grammar_text)
    unknown = (set(reserved) | {record[0] for record in binary}) - set(names)
    if unknown:
        raise SurfaceError(f"grammar catalog references unknown tokens: {sorted(unknown)}")
    if len(binary) != len({record[0] for record in binary}):
        raise SurfaceError("grammar catalog contains duplicate binary operators")
    group_contract = {}
    precedences = set()
    for _, group, precedence, associativity in binary:
        contract = (int(precedence), associativity)
        if group in group_contract and group_contract[group] != contract:
            raise SurfaceError(f"precedence group {group} has conflicting metadata")
        group_contract[group] = contract
        precedences.add(int(precedence))
    if precedences != set(range(1, max(precedences, default=0) + 1)):
        raise SurfaceError("binary precedence levels must be contiguous from one")

    parser_dir = root / "lib/Compiler/Frontend"
    parser_text = (parser_dir / "Parser.cpp").read_text(encoding="utf-8")
    parser_text += "\n" + "\n".join(
        path.read_text(encoding="utf-8")
        for path in sorted(parser_dir.glob("Parser*.inc"))
    )
    if "binaryOperatorInfo(token().kind)" not in parser_text or \
            "static int precedence(" in parser_text:
        raise SurfaceError("Parser.cpp must consume the shared precedence catalog")
    for token in reserved:
        if f"TokenKind::{token}" in parser_text:
            raise SurfaceError(f"reserved token {token} is admitted by Parser.cpp")

    lexer_text = (root / "lib/Compiler/Frontend/Lexer.cpp").read_text(encoding="utf-8")
    if any(name in lexer_text for name in ("std::isalpha", "std::isalnum",
                                            "std::isdigit", "std::isspace")):
        raise SurfaceError("compiler lexer must not use locale-sensitive character classes")


def validate_unsafe_authority(root: Path) -> None:
    catalog = (root / "include/chtholly/Compiler/UnsafeAuthority.def").read_text(
        encoding="utf-8")
    records = re.findall(
        r"CHTHOLLY_COMPILER_UNSAFE_OPERATION\(\s*([A-Za-z0-9_]+),\s*"
        r"([A-Za-z0-9_]+),\s*\"([a-z0-9-]+)\"\s*\)", catalog)
    if len(records) < 8 or len(records) != len({record[0] for record in records}):
        raise SurfaceError("unsafe operation catalog is incomplete or duplicated")
    diagnostics = definitions(root / "include/chtholly/Compiler/DiagnosticKind.def",
                              "CHTHOLLY_COMPILER_DIAGNOSTIC")
    unknown = {record[1] for record in records} - diagnostics
    if unknown:
        raise SurfaceError(
            f"unsafe operation catalog references unknown diagnostics: {sorted(unknown)}")
    semantics = semantic_sources(root)
    if "unsafe_depth_ == 0" in semantics or semantics.count("unsafe_depth_ != 0") != 1:
        raise SurfaceError("unsafe authority checks must use requireUnsafe")


def validate_program_model(root: Path) -> None:
    model = (root / "include/chtholly/Compiler/ProgramModel.h").read_text(
        encoding="utf-8")
    for contract in ('V1SourceEntryName = "main"',
                     'V1EmbeddedEntrySymbol = "chtholly.entry"',
                     'windows ? "wmain" : "main"'):
        if contract not in model:
            raise SurfaceError(f"program model omits hosted contract: {contract}")
    semantics = semantic_sources(root)
    llvm = "\n".join(path.read_text(encoding="utf-8") for path in (
        root / "lib/Compiler/Lowering/LLVM.cpp",
        root / "lib/Compiler/Lowering/LLVMModuleLowering.cpp",
        root / "lib/Compiler/Lowering/LLVMEntryPointLowering.cpp"))
    driver = "\n".join(path.read_text(encoding="utf-8") for path in (
        root / "lib/Driver/Compiler/CompilerPipeline.cpp",
        root / "lib/Driver/Compiler/CompilerPipelineSourceInspection.cpp",
        root / "lib/Driver/Compiler/CompilerPipelineSupportInternal.h"))
    if semantics.count("isV1SourceEntryName(function_text)") < 3 or \
            "v1HostedEntrySymbol(windows)" not in llvm or \
            "V1EmbeddedEntrySymbol" not in llvm:
        raise SurfaceError("frontend and LLVM must consume ProgramModel.h")
    if 'module_name == "std"' not in driver or \
            'module_name.starts_with("std::")' not in driver or \
            "if (needs_standard_library)" not in driver:
        raise SurfaceError("standard library must be loaded only by explicit import")


def definitions(path: Path, macro: str) -> set[str]:
    pattern = re.compile(rf"{re.escape(macro)}\(([A-Za-z0-9_]+),?")
    return set(pattern.findall(path.read_text(encoding="utf-8")))


def heading_slug(line: str) -> str:
    heading = line.lstrip("# ").strip().lower()
    heading = re.sub(r"[^a-z0-9 -]", "", heading)
    return re.sub(r"[ -]+", "-", heading).strip("-")


def read_manifest(root: Path, relative: str, seen: set[str] | None = None):
    seen = set() if seen is None else seen
    if relative in seen:
        raise SurfaceError(f"surface manifest inheritance cycle at {relative}")
    seen.add(relative)
    data = tomllib.loads((root / relative).read_text(encoding="utf-8"))
    schema = data.get("schema")
    if schema == SCHEMA_V2:
        if data.get("release") != "1.0" or data.get("base"):
            raise SurfaceError(f"base surface manifest {relative} must describe 1.0")
        return data
    if schema != SCHEMA_V3:
        raise SurfaceError(f"surface manifest {relative} has unsupported schema")
    base_relative = data.get("base", "")
    if not base_relative:
        raise SurfaceError(f"candidate surface manifest {relative} requires base")
    base = read_manifest(root, base_relative, seen)
    if base.get("status") != "frozen":
        raise SurfaceError("a candidate surface must inherit a frozen base")
    base_features = list(base.get("feature", []))
    indexes = {feature["id"]: index for index, feature in enumerate(base_features)}
    for feature in data.get("feature", []):
        identifier = feature.get("id", "")
        if identifier in indexes:
            inherited = base_features[indexes[identifier]]
            if not str(inherited.get("scope", "")).startswith("post-v"):
                raise SurfaceError(
                    f"candidate surface cannot replace frozen feature {identifier}")
            base_features[indexes[identifier]] = feature
        else:
            indexes[identifier] = len(base_features)
            base_features.append(feature)
    admitted_spellings = data.get("admitted_compatibility_spellings", [])
    inherited_forbidden = base.get("forbidden_compatibility_spellings", [])
    if not isinstance(admitted_spellings, list) or \
            any(not isinstance(spelling, str) or not spelling
                for spelling in admitted_spellings) or \
            len(set(admitted_spellings)) != len(admitted_spellings):
        raise SurfaceError(
            "derived surface admitted spellings must be unique strings")
    if any(spelling not in inherited_forbidden for spelling in admitted_spellings):
        raise SurfaceError(
            "derived surface may admit only a spelling forbidden by its base")
    merged = dict(base)
    merged.update({key: value for key, value in data.items()
                   if key not in {"base", "feature", "normative_specs",
                                  "forbidden_compatibility_spellings",
                                  "admitted_compatibility_spellings"}})
    merged["feature"] = base_features
    merged["normative_specs"] = list(dict.fromkeys(
        [*base.get("normative_specs", []), *data.get("normative_specs", [])]))
    merged["forbidden_compatibility_spellings"] = list(dict.fromkeys(
        [*(spelling for spelling in inherited_forbidden
           if spelling not in admitted_spellings),
         *data.get("forbidden_compatibility_spellings", [])]))
    merged["manifest_path"] = relative
    merged["base_path"] = base_relative
    return merged


def load_evidence_matrix(root: Path, data: dict) -> dict[str, dict]:
    relative = data.get("evidence_matrix", "")
    if not relative:
        raise SurfaceError("surface manifest requires an executable evidence matrix")
    matrix = tomllib.loads((root / relative).read_text(encoding="utf-8"))
    if matrix.get("schema") != "chtholly-feature-evidence-v1":
        raise SurfaceError("feature evidence matrix has an unsupported schema")
    if tuple(matrix.get("categories", [])) != EVIDENCE_CATEGORIES:
        raise SurfaceError("feature evidence matrix categories are incomplete or reordered")

    test_manifest = (root / "tests/chtholly-tests.toml.in").read_text(
        encoding="utf-8")
    registered = set(TEST_NAME_RECORD.findall(test_manifest))
    forbidden_evidence = ("surface_contract", "boundary_audit", "framework")
    evidence: dict[str, dict] = {}
    group_ids: set[str] = set()
    for group in matrix.get("coverage", []):
        group_id = group.get("id", "")
        if not ID.fullmatch(group_id) or group_id in group_ids:
            raise SurfaceError(f"invalid or duplicate evidence group: {group_id!r}")
        group_ids.add(group_id)
        for category in EVIDENCE_CATEGORIES:
            tests = group.get(category, [])
            if not isinstance(tests, list) or not tests or any(
                    not isinstance(test, str) or not test for test in tests):
                raise SurfaceError(
                    f"evidence group {group_id} requires {category} tests")
            unknown = set(tests) - registered
            if unknown:
                raise SurfaceError(
                    f"evidence group {group_id} references unregistered tests: "
                    f"{sorted(unknown)}")
            forbidden = [test for test in tests
                         if any(marker in test for marker in forbidden_evidence)]
            if forbidden:
                raise SurfaceError(
                    f"evidence group {group_id} uses static/framework evidence "
                    f"for {category}: {forbidden}")
        features = group.get("features", [])
        if not isinstance(features, list) or not features:
            raise SurfaceError(f"evidence group {group_id} has no features")
        for identifier in features:
            if identifier in evidence:
                raise SurfaceError(
                    f"feature {identifier} appears in multiple evidence groups")
            evidence[identifier] = {
                "group": group_id,
                **{category: list(group[category])
                   for category in EVIDENCE_CATEGORIES},
            }

    complete = {
        feature["id"] for feature in data.get("feature", [])
        if feature.get("implementation") == "complete"
    }
    missing = complete - set(evidence)
    if missing:
        raise SurfaceError(
            "complete features lack categorized executable evidence: "
            f"{sorted(missing)}")
    if data.get("release") == matrix.get("release"):
        extra = set(evidence) - complete
        if extra:
            raise SurfaceError(
                f"feature evidence matrix contains unknown complete features: {sorted(extra)}")
    return evidence


def load(root: Path, manifest: str = MANIFEST):
    validate_grammar(root)
    validate_unsafe_authority(root)
    validate_program_model(root)
    data = read_manifest(root, manifest)
    release = data.get("release", "")
    if not re.fullmatch(r"[1-9][0-9]*\.[0-9]+", release):
        raise SurfaceError("manifest release must use canonical MAJOR.MINOR")
    if data.get("status") not in {"candidate", "frozen"}:
        raise SurfaceError("manifest status must be candidate or frozen")
    if data.get("compiler") != "chtholly":
        raise SurfaceError("Chtholly language surfaces must name chtholly as compiler")
    major, minor = (int(part) for part in release.split("."))
    current_scope = "v1" if release == "1.0" else f"v{release}"
    post_scope = "post-v1" if release == "1.0" else f"post-v{release}"
    allowed_scopes = {current_scope, post_scope}
    if release != "1.0":
        allowed_scopes.update({"v1", "post-v1"})
        allowed_scopes.update(f"v{major}.{version_minor}"
                              for version_minor in range(1, minor))

    specs = data.get("normative_specs", [])
    if not specs:
        raise SurfaceError("manifest requires normative specifications")
    for relative in specs:
        if not (root / relative).is_file():
            raise SurfaceError(f"missing normative specification: {relative}")

    tokens = definitions(root / "include/chtholly/Compiler/TokenKind.def",
                         "CHTHOLLY_COMPILER_TOKEN")
    nodes = definitions(root / "include/chtholly/Compiler/NodeKind.def",
                        "CHTHOLLY_COMPILER_NODE")
    identifiers = set()
    candidate_gaps = 0
    for feature in data.get("feature", []):
        identifier = feature.get("id", "")
        scope = feature.get("scope")
        design = feature.get("design")
        implementation = feature.get("implementation")
        if not ID.fullmatch(identifier) or identifier in identifiers:
            raise SurfaceError(f"invalid or duplicate feature id: {identifier!r}")
        if scope not in allowed_scopes:
            raise SurfaceError(f"feature {identifier} has invalid scope")
        if design not in {"normative", "pending"}:
            raise SurfaceError(f"feature {identifier} has invalid design status")
        if implementation not in {"complete", "partial", "none"}:
            raise SurfaceError(
                f"feature {identifier} has invalid implementation status")
        identifiers.add(identifier)
        if scope == current_scope and (design != "normative" or
                                       implementation != "complete"):
            candidate_gaps += 1
        document, _, anchor = feature.get("spec", "").partition("#")
        spec_path = root / document
        if not document or not anchor or not spec_path.is_file():
            raise SurfaceError(f"feature {identifier} has invalid spec anchor")
        spec_text = spec_path.read_text(encoding="utf-8")
        if not any(heading_slug(line) == anchor
                   for line in spec_text.splitlines() if line.startswith("#")):
            raise SurfaceError(f"feature {identifier} references missing anchor")
        unknown_tokens = set(feature.get("tokens", [])) - tokens
        unknown_nodes = set(feature.get("nodes", [])) - nodes
        if unknown_tokens or unknown_nodes:
            raise SurfaceError(
                f"feature {identifier} references missing compiler definitions: "
                f"tokens={sorted(unknown_tokens)}, nodes={sorted(unknown_nodes)}")
        evidence = feature.get("evidence", [])
        if implementation in {"complete", "partial"} and not evidence:
            raise SurfaceError(
                f"implemented feature {identifier} requires evidence")
        if implementation == "none" and evidence:
            raise SurfaceError(
                f"unimplemented feature {identifier} cannot claim evidence")
        for relative in evidence:
            if not (root / relative).is_file():
                raise SurfaceError(f"feature {identifier} has missing evidence: {relative}")
    if not identifiers:
        raise SurfaceError("manifest requires features")

    wave_ids = set()
    scheduled = set()
    for wave in data.get("closure_wave", []):
        wave_id = wave.get("id", "")
        if not ID.fullmatch(wave_id) or wave_id in wave_ids:
            raise SurfaceError(f"invalid or duplicate closure wave id: {wave_id!r}")
        if not wave.get("title"):
            raise SurfaceError(f"closure wave {wave_id} requires a title")
        features = wave.get("features", [])
        if not features:
            raise SurfaceError(f"closure wave {wave_id} requires features")
        wave_ids.add(wave_id)
        for identifier in features:
            if identifier not in identifiers:
                raise SurfaceError(
                    f"closure wave {wave_id} references unknown feature {identifier}")
            if identifier in scheduled:
                raise SurfaceError(
                    f"feature {identifier} appears in more than one closure wave")
            feature = next(item for item in data["feature"]
                           if item["id"] == identifier)
            if (feature["scope"] != current_scope or
                    (feature["design"] == "normative" and
                     feature["implementation"] == "complete")):
                raise SurfaceError(
                    f"closure wave {wave_id} contains closed or post-v1 feature "
                    f"{identifier}")
            scheduled.add(identifier)
    incomplete = {
        feature["id"] for feature in data["feature"]
        if feature["scope"] == current_scope and
        (feature["design"] != "normative" or
         feature["implementation"] != "complete")
    }
    if scheduled != incomplete:
        raise SurfaceError(
            f"closure waves must schedule every incomplete {release} feature exactly once: "
            f"missing={sorted(incomplete - scheduled)}")

    if data["status"] == "candidate" and candidate_gaps == 0:
        raise SurfaceError(
            f"candidate manifest requires a {release} design or implementation gap")
    if data["status"] == "frozen" and candidate_gaps:
        raise SurfaceError(
            f"a frozen {release} manifest requires every current feature to be normative and complete")

    for spelling in data.get("forbidden_compatibility_spellings", []):
        checked_paths = [root / relative for relative in specs]
        examples = root / "docs/spec/examples"
        if examples.is_dir():
            checked_paths.extend(path for path in sorted(examples.rglob("*"))
                                 if path.is_file())
        for path in checked_paths:
            if path.suffix in {".md", ".cns"} and \
                    spelling in path.read_text(encoding="utf-8"):
                raise SurfaceError(
                    f"forbidden compatibility spelling {spelling!r} in "
                    f"{path.relative_to(root)}")
    data["categorized_evidence"] = load_evidence_matrix(root, data)
    return data


def render(data: dict, manifest: str = MANIFEST) -> str:
    release = data["release"]
    version_label = "v1" if release == "1.0" else release
    lines = [
        f"# Chtholly {version_label} Surface Status",
        "",
        f"Generated from `{manifest}`; do not edit.",
        "",
        f"Release status: **{data['status']}**.",
        "",
        "| Feature | Scope | Design | Implementation | Specification | Evidence |",
        "| --- | --- | --- | --- | --- | --- |",
    ]
    for feature in data["feature"]:
        categorized = data.get("categorized_evidence", {}).get(feature["id"])
        evidence = ""
        if categorized:
            evidence = "; ".join(
                f"{category}: " + ", ".join(
                    f"`{test}`" for test in categorized[category])
                for category in EVIDENCE_CATEGORIES)
        lines.append(
            f"| `{feature['id']}`: {feature['title']} | {feature['scope']} | "
            f"{feature['design']} | {feature['implementation']} | "
            f"`{feature['spec']}` | {evidence or '-'} |")
    lines.extend([
        "",
        f"## Remaining {version_label} Closure Order",
        "",
        "Waves are ordered; entries within one wave may close together.",
        "",
    ])
    waves = data.get("closure_wave", [])
    for index, wave in enumerate(waves, 1):
        features = ", ".join(f"`{identifier}`"
                             for identifier in wave["features"])
        lines.append(f"{index}. **{wave['title']}**: {features}")
    if not waves:
        lines.append(f"No remaining {version_label} closure waves.")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-dir", type=Path,
                        default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--manifest", default=MANIFEST)
    parser.add_argument("--generated", default=GENERATED)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        root = args.source_dir.resolve()
        data = load(root, args.manifest)
        output = root / args.generated
        expected = render(data, args.manifest)
        if args.check:
            if not output.is_file() or output.read_text(encoding="utf-8") != expected:
                raise SurfaceError(f"generated file is stale: {args.generated}")
        else:
            output.write_text(expected, encoding="utf-8", newline="\n")
    except (OSError, KeyError, tomllib.TOMLDecodeError, SurfaceError) as error:
        print(f"chtholly-v1-surface: {error}", file=sys.stderr)
        return 1
    print(f"chtholly-language-surface={data['release']}:{data['status']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
