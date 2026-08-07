#!/usr/bin/env python3
"""Structural dependency scan over the engine and sandbox trees.

Walks BOTH source trees in a single pass so that include edges crossing the
sandbox -> engine boundary are resolved against the same file table as
same-side edges (scanning the trees independently loses exactly those edges).

Emits docs/architecture/_raw/dependency-graph.json. This pass is purely
structural: no attempt is made to describe what any file does.

Build-target attribution is read from the actual build scripts, not guessed:

  engine/build.bat   FOR /R source %%f in (*.cpp)      -> engine.dll  (target "engine")
                     -Isource -Ivendor/include
  sandbox/build.bat  FOR /R %%f in (*.cpp)             -> sandbox.exe (target "sandbox")
                     -Isource -I../engine/source/
                     links -L../bin/ -lengine.lib

Vendored third-party trees (engine/vendor) and build intermediates
(engine/obj) are excluded: they are not project code, and their includes are
resolved as external.
"""

from __future__ import annotations

import json
import re
import sys
from collections import defaultdict
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / "docs" / "architecture" / "_raw" / "dependency-graph.json"

SOURCE_EXT = {".cpp", ".h", ".hpp"}

# Directories that hold third-party code or build intermediates, relative to REPO.
EXCLUDED_DIRS = {"vendor", "obj", "bin", ".git", "build"}

# Per-side include search paths, transcribed from the build scripts' -I flags.
# Order matters: the compiler searches them in this order after the quoted-include
# "directory of the including file" rule.
INCLUDE_DIRS = {
    "engine": ["engine/source"],
    "sandbox": ["sandbox/source", "engine/source"],
}

# Which build target each tree's translation units land in. Both build scripts
# glob their whole source tree, so tree membership determines the target.
TARGETS = {
    "engine": "engine",     # engine.dll  (shared library)
    "sandbox": "sandbox",   # sandbox.exe (links engine.lib)
}

TREE_ROOTS = {
    "engine": "engine/source",
    "sandbox": "sandbox/source",
}

# #include "foo.h" / #include <foo.h>, ignoring lines commented out with //.
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*(?:"([^"]+)"|<([^>]+)>)')
BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
LINE_COMMENT_RE = re.compile(r"//[^\n]*")


def is_excluded(rel: Path) -> bool:
    return any(part in EXCLUDED_DIRS for part in rel.parts)


def side_of(rel_posix: str) -> str | None:
    for side, root in TREE_ROOTS.items():
        if rel_posix == root or rel_posix.startswith(root + "/"):
            return side
    return None


def collect_files() -> dict[str, str]:
    """Return {repo-relative posix path: side} for every project source file."""
    files: dict[str, str] = {}
    for side, root in TREE_ROOTS.items():
        base = REPO / root
        if not base.is_dir():
            print(f"warning: missing tree {root}", file=sys.stderr)
            continue
        for path in base.rglob("*"):
            if not path.is_file() or path.suffix.lower() not in SOURCE_EXT:
                continue
            rel = path.relative_to(REPO)
            if is_excluded(rel):
                continue
            files[rel.as_posix()] = side
    return files


def strip_comments(text: str) -> str:
    """Drop commented-out includes so they are not counted as real edges.

    String literals containing "//" are not a concern here: we only care about
    the shape of #include lines, and a stray truncation cannot invent an edge.
    """
    text = BLOCK_COMMENT_RE.sub("", text)
    return LINE_COMMENT_RE.sub("", text)


def read_includes(path: Path) -> list[tuple[str, bool]]:
    """Return [(included spelling, was_quoted)] in source order."""
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        text = path.read_text(encoding="utf-8", errors="replace")
    out: list[tuple[str, bool]] = []
    for line in strip_comments(text).splitlines():
        m = INCLUDE_RE.match(line)
        if not m:
            continue
        quoted, angled = m.group(1), m.group(2)
        out.append((quoted, True) if quoted else (angled, False))
    return out


def resolve_include(
    spelling: str, quoted: bool, including: str, side: str, files: dict[str, str]
) -> str | None:
    """Resolve an include spelling to a project file, or None if external.

    Mirrors the compiler's search: for quoted includes, the directory of the
    including file first; then the -I directories for that side. Angle includes
    are resolved the same way minus the relative rule, so a project header
    pulled in with <> still registers as an internal edge.
    """
    spelling = spelling.replace("\\", "/")
    candidates: list[str] = []
    if quoted:
        candidates.append((Path(including).parent / spelling).as_posix())
    for inc_dir in INCLUDE_DIRS[side]:
        candidates.append(f"{inc_dir}/{spelling}")
    for cand in candidates:
        norm = Path(cand).as_posix()
        # Normalise any ../ segments without touching the filesystem.
        parts: list[str] = []
        for part in norm.split("/"):
            if part in ("", "."):
                continue
            if part == "..":
                if parts:
                    parts.pop()
                continue
            parts.append(part)
        norm = "/".join(parts)
        if norm in files:
            return norm
    return None


def main() -> int:
    files = collect_files()
    if not files:
        print("error: no source files found", file=sys.stderr)
        return 1

    includes: dict[str, list[str]] = {}
    unresolved: dict[str, list[str]] = defaultdict(list)
    fan_in: dict[str, int] = {f: 0 for f in files}

    for rel, side in sorted(files.items()):
        resolved: list[str] = []
        seen: set[str] = set()
        for spelling, quoted in read_includes(REPO / rel):
            target = resolve_include(spelling, quoted, rel, side, files)
            if target is None:
                unresolved[rel].append(spelling)
                continue
            if target == rel or target in seen:
                continue  # self-include or duplicate: one edge per pair
            seen.add(target)
            resolved.append(target)
        includes[rel] = resolved

    for src, deps in includes.items():
        for dep in deps:
            fan_in[dep] += 1

    boundary_edges = []
    for src, deps in sorted(includes.items()):
        for dep in deps:
            src_side, dep_side = files[src], files[dep]
            if src_side == dep_side:
                continue
            edge = {"from": src, "to": dep, "direction": f"{src_side}->{dep_side}"}
            if src_side == "engine":
                # An engine file reaching into sandbox inverts the dependency
                # direction the build enforces (sandbox links engine, not the
                # reverse) and would not even compile under engine/build.bat's
                # include paths.
                edge["anomaly"] = True
                edge["reason"] = "engine file includes a sandbox header (reverse dependency)"
            boundary_edges.append(edge)

    out = {
        "_meta": {
            "repo_root": REPO.as_posix(),
            "generated_by": "tools/dependency_graph/scan_dependencies.py",
            "build_system": "clang++ driven by build-all.bat -> engine/build.bat, sandbox/build.bat",
            "targets": {
                "engine": {
                    "artifact": "bin/engine.dll",
                    "sources": "engine/source/**/*.cpp",
                    "include_dirs": INCLUDE_DIRS["engine"],
                },
                "sandbox": {
                    "artifact": "bin/sandbox.exe",
                    "sources": "sandbox/source/**/*.cpp",
                    "include_dirs": INCLUDE_DIRS["sandbox"],
                    "links": ["engine.lib"],
                },
            },
            "excluded_dirs": sorted(EXCLUDED_DIRS),
            "file_count": len(files),
            "edge_count": sum(len(v) for v in includes.values()),
            "boundary_edge_count": len(boundary_edges),
            "anomaly_count": sum(1 for e in boundary_edges if e.get("anomaly")),
            "unresolved_includes": {k: sorted(set(v)) for k, v in sorted(unresolved.items())},
        },
        "files": {
            rel: {
                "side": files[rel],
                "includes": includes[rel],
                "target": TARGETS[files[rel]],
                "fan_in": fan_in[rel],
                "fan_out": len(includes[rel]),
            }
            for rel in sorted(files)
        },
        "boundary_edges": boundary_edges,
    }

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")

    print(f"files:          {len(files)}")
    print(f"internal edges: {out['_meta']['edge_count']}")
    print(f"boundary edges: {len(boundary_edges)} "
          f"({out['_meta']['anomaly_count']} anomalous)")
    print(f"wrote {OUT.relative_to(REPO).as_posix()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
