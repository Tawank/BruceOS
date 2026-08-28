#!/usr/bin/env python3
"""Generates docs/bruce_sdk.md from the Doxygen-style comments in src/core_sdk/*.h.

This is a small, purpose-built parser (not a wrapper around the `doxygen`
binary): core_sdk headers are plain C, and the doc comments follow a simple,
consistent convention rather than the full Doxygen comment grammar. Keeping
the parser here as one dependency-free Python script means anyone can
regenerate the docs without installing Doxygen.

Convention the parser understands, per header file:

  * The first `/** ... */` block in the file is the module-level doc. It
    should contain a `@brief <one line>` tag and, optionally, a longer
    description in the following paragraph(s).

  * Every other `/** ... */` block documents the declaration(s) that
    immediately follow it, with no blank line in between. If several
    declarations in a row have no comment of their own, they inherit the
    nearest preceding `/** ... */` block above them; a blank line resets
    that inheritance, so an undocumented declaration after a blank line is
    rendered without a description instead of borrowing an unrelated one.
    Each function is rendered as its own section in the output, so a
    doc comment that is meant to cover several functions with *different*
    parameters should normally be split into one block per function instead
    (each repeating `@permission`/shared prose as needed) -- see ssh.h for
    the pattern.

  * A `@brief <one line>` tag is the one-line summary; a blank line then
    separates it from an optional longer description.

  * `@param <name> <description>` documents one function parameter. One
    per line (no wrapping). Every parameter of a documented function should
    have one; the generator renders whatever it finds and leaves the
    description blank for parameters it has no tag for.

  * A `@permission <names or note>` tag records what core_sdk/permission.h
    permission(s) (see BRUCE_PERMISSION_*) the declaration checks, e.g.
    `@permission wifi, config`. Special values `none` and `built-in only`
    are used where the header text says so explicitly. The tag is omitted
    entirely where the header comments don't state a permission
    requirement one way or the other -- the generated docs never guess.

  * Plain `/* ... */` (single star) comments are never treated as
    documentation; they're left as ordinary source comments (section
    dividers, inline struct-field notes, ...) and skipped.

  * `#define` lines are collected into a per-file "Constants" table
    instead of getting their own doc entry, since they're simple value
    constants rather than documented API surface.

Usage: tools/gen_sdk_docs.py [--check]
  --check   Exit non-zero if docs/bruce_sdk.md would change (CI use).
"""

import argparse
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDK_DIR = os.path.join(ROOT, "src", "core_sdk")
OUTPUT_PATH = os.path.join(ROOT, "docs", "bruce_sdk.md")

# Special @permission values that don't name an actual bruce_permission_t.
NON_PERMISSION_TOKENS = {"none", "built-in only"}


class DocComment:
    """A parsed `/** ... */` block."""

    def __init__(self, raw_lines):
        content_lines = []
        for line in raw_lines:
            stripped = line.strip()
            if stripped.startswith("/**"):
                stripped = stripped[3:].strip()
            elif stripped.startswith("*/"):
                stripped = stripped[2:].strip()
            elif stripped.startswith("*"):
                stripped = stripped[1:]
                if stripped.startswith(" "):
                    stripped = stripped[1:]
            if stripped.endswith("*/"):
                stripped = stripped[:-2].rstrip()
            content_lines.append(stripped.rstrip())
        text = "\n".join(content_lines).strip("\n")

        self.brief = ""
        self.permission = None
        self.params = []  # list of (name, description)

        brief_match = re.search(r"@brief\s+(.+?)(?:\n[ \t]*\n|\n@|\Z)", text, re.DOTALL)
        if brief_match:
            self.brief = " ".join(brief_match.group(1).split())
            text = text[: brief_match.start()] + text[brief_match.end() :]

        for m in re.finditer(r"^@param\s+(\S+)\s+(.+)$", text, re.MULTILINE):
            self.params.append((m.group(1).strip(), m.group(2).strip()))
        text = re.sub(r"^@param\s+.+$\n?", "", text, flags=re.MULTILINE)

        perm_match = re.search(r"^@permission\s+(.+)$", text, re.MULTILINE)
        if perm_match:
            self.permission = perm_match.group(1).strip()
            text = text[: perm_match.start()] + text[perm_match.end() :]

        # Collapse leftover blank-line runs left behind by tag removal.
        text = re.sub(r"\n{3,}", "\n\n", text).strip("\n")
        self.body = text

    def permission_tokens(self):
        if not self.permission:
            return []
        # Drop parenthetical notes like "(external callers only; ...)".
        cleaned = re.sub(r"\([^)]*\)", "", self.permission)
        tokens = [t.strip() for t in cleaned.split(",")]
        return [t for t in tokens if t]

    def param_desc(self, name):
        for pname, pdesc in self.params:
            if pname == name:
                return pdesc
        return ""


class Declaration:
    def __init__(self, name, code, doc):
        self.name = name
        self.code = code
        self.doc = doc


class Constant:
    def __init__(self, name, value):
        self.name = name
        self.value = value


class Module:
    def __init__(self, filename):
        self.filename = filename
        self.brief = ""
        self.body = ""
        self.constants = []
        self.declarations = []


def extract_name(code):
    """Best-effort declaration name for a heading, from its joined source text."""
    # Function-pointer typedef: typedef ... (*name)(...);
    m = re.search(r"\(\*\s*(\w+)\s*\)\s*\(", code)
    if m:
        return m.group(1)
    # typedef struct/enum { ... } name;
    m = re.search(r"\}\s*(\w+)\s*;\s*$", code, re.DOTALL)
    if m:
        return m.group(1)
    # Plain function declaration: name is the identifier before the first '('.
    paren = code.find("(")
    if paren != -1:
        m = re.search(r"(\w+)\s*$", code[:paren])
        if m:
            return m.group(1)
    # Plain typedef/variable-style declaration: name is the identifier before ';'.
    m = re.search(r"(\w+)\s*;\s*$", code, re.DOTALL)
    if m:
        return m.group(1)
    return code.strip().splitlines()[0][:40]


def split_params(param_str):
    """Splits a parameter-list string on top-level commas (nesting-aware)."""
    param_str = param_str.strip()
    if param_str == "" or param_str == "void":
        return []
    parts = []
    depth = 0
    cur = ""
    for ch in param_str:
        if ch in "([<":
            depth += 1
        elif ch in ")]>":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    parts.append(cur)
    return [p.strip() for p in parts if p.strip()]


def parse_param(token):
    """Splits one `TYPE NAME` parameter token into (name, display_type)."""
    if token == "...":
        return "...", ""
    arr_suffix = ""
    m = re.search(r"(\[[^\]]*\])\s*$", token)
    if m:
        arr_suffix = m.group(1)
        token = token[: m.start()].rstrip()
    m = re.search(r"([A-Za-z_]\w*)\s*$", token)
    if not m:
        return token, ""
    name = m.group(1)
    type_ = token[: m.start()].rstrip()
    if arr_suffix:
        type_ = f"{type_}{arr_suffix}" if type_.endswith("*") else f"{type_} {arr_suffix}".strip()
    return name, type_


RETURN_QUALIFIER_RE = re.compile(r"^(?:static|inline)\s+")


def parse_signature(name, code):
    """For a function declaration, returns (return_type, [(name, type), ...])
    or None if `code` doesn't look like a function declaration/definition."""
    flat = " ".join(code.split())
    if flat.startswith("typedef"):
        return None
    m = re.search(r"(?<!\w)" + re.escape(name) + r"\s*\(", flat)
    if not m:
        return None

    start = m.end() - 1
    depth = 0
    end = None
    for i in range(start, len(flat)):
        if flat[i] == "(":
            depth += 1
        elif flat[i] == ")":
            depth -= 1
            if depth == 0:
                end = i
                break
    if end is None:
        return None

    return_type = flat[: m.start()].strip()
    while True:
        new_return_type = RETURN_QUALIFIER_RE.sub("", return_type)
        if new_return_type == return_type:
            break
        return_type = new_return_type

    params = [parse_param(p) for p in split_params(flat[start + 1 : end])]
    return return_type, params


def heading_name(name):
    """Renders `module__function` as `module.function()`, else `name()`."""
    if "__" in name:
        module, rest = name.split("__", 1)
        return f"{module}.{rest}()"
    return f"{name}()"


SKIP_LINE_RE = re.compile(
    r'^\s*(#include\b|#pragma\b|#ifdef __cplusplus\s*$|#endif\s*$|extern "C" \{\s*$)'
)


def parse_file(path):
    filename = os.path.basename(path)
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()

    module = Module(filename)
    n = len(lines)

    # Single pass over the whole file (any #define/typedef that happens to
    # appear before the first /** ... */ block, as in pubsub.h, must still be
    # captured -- so this does not skip ahead looking for that block first).
    # The first /** ... */ block encountered is the file-level doc; it also
    # becomes the initial pending_doc, in case a declaration immediately
    # follows it with no blank line in between.
    file_doc_consumed = False
    pending_doc = None
    i = 0
    while i < n:
        raw = lines[i]
        stripped = raw.strip()

        if stripped == "":
            pending_doc = None
            i += 1
            continue

        if SKIP_LINE_RE.match(raw):
            i += 1
            continue

        if stripped == "}" or stripped == "};":
            # Stray brace (e.g. closing `extern "C" {`) outside any
            # declaration we're accumulating below.
            i += 1
            continue

        if "/**" in stripped:
            block = [raw]
            while "*/" not in lines[i]:
                i += 1
                block.append(lines[i])
            i += 1
            doc = DocComment(block)
            if not file_doc_consumed:
                module.brief = doc.brief
                module.body = doc.body
                file_doc_consumed = True
            pending_doc = doc
            continue

        if stripped.startswith("/*"):
            # Plain (non-doc) comment: skip without touching pending_doc.
            block = [raw]
            while "*/" not in lines[i]:
                i += 1
                block.append(lines[i])
            i += 1
            continue

        if stripped.startswith("//"):
            i += 1
            continue

        define_match = re.match(r"#define\s+(\S+)\s*(.*)$", stripped)
        if define_match:
            name, value = define_match.groups()
            module.constants.append(Constant(name, value.strip()))
            i += 1
            continue

        if stripped.startswith("#"):
            # Any other preprocessor directive we don't specially handle.
            i += 1
            continue

        # Otherwise: this begins a declaration. Accumulate lines until it is
        # syntactically complete (balanced braces/parens and a trailing ';').
        decl_lines = [raw]
        brace_depth = raw.count("{") - raw.count("}")
        paren_depth = raw.count("(") - raw.count(")")

        def is_complete(text, braces, parens):
            # A plain prototype ends with ';'. A `static inline ... { ... }`
            # one-liner (e.g. input__poll()) has an inline body and ends
            # with '}' instead, once its braces/parens are balanced.
            end = text.rstrip()
            return braces <= 0 and parens <= 0 and (end.endswith(";") or end.endswith("}"))

        while not is_complete(decl_lines[-1], brace_depth, paren_depth):
            i += 1
            if i >= n:
                break
            decl_lines.append(lines[i])
            brace_depth += lines[i].count("{") - lines[i].count("}")
            paren_depth += lines[i].count("(") - lines[i].count(")")
        i += 1

        code = "\n".join(decl_lines).strip()
        name = extract_name(code)
        module.declarations.append(Declaration(name, code, pending_doc))

    return module


def github_slug(text):
    """Approximates GitHub's heading-anchor slug algorithm (used by our TOC links)."""
    slug = re.sub(r"[^\w\s-]", "", text.lower())
    return re.sub(r"\s+", "-", slug.strip())


def render_permission_block(doc):
    lines = ["#### Permissions\n"]
    tokens = doc.permission_tokens()
    real_tokens = [t for t in tokens if t not in NON_PERMISSION_TOKENS]
    notes = [t for t in tokens if t in NON_PERMISSION_TOKENS]
    if not real_tokens and "none" in notes:
        lines.append("_Not permission-gated._\n")
    else:
        for t in real_tokens:
            lines.append(f"- `{t}`")
        for t in notes:
            if t == "built-in only":
                lines.append("- _Built-in apps only -- external ELF/JS/WASM callers are always denied._")
            elif t == "none":
                continue
        lines.append("")
    return lines


def render_declaration(decl):
    """Renders one declaration (function or type/constant-like) as a list of
    markdown lines, in the `## name()` / `### Parameters` / ... style."""
    sig = parse_signature(decl.name, decl.code)
    out = []
    out.append(f"## {heading_name(decl.name)}\n")
    out.append("```c")
    out.append(decl.code)
    out.append("```\n")

    doc = decl.doc
    if doc and doc.brief:
        out.append(doc.brief + "\n")
    if doc and doc.body:
        out.append(doc.body + "\n")

    if sig is not None:
        return_type, params = sig
        if params:
            out.append("### Parameters\n")
            out.append("| Parameter | Type | Description |")
            out.append("| --- | --- | --- |")
            for pname, ptype in params:
                desc = doc.param_desc(pname) if doc else ""
                ptype_cell = ptype.replace("|", "\\|") if ptype else ""
                desc_cell = desc.replace("|", "\\|")
                out.append(f"| `{pname}` | `{ptype_cell}` | {desc_cell} |")
            out.append("")
        out.append("### Returns\n")
        out.append(f"`{return_type}`\n")

    if doc and doc.permission:
        out.extend(render_permission_block(doc))

    return out


def render_module(module, out):
    out.append(f"# `{module.filename}`\n")
    if module.brief:
        out.append(f"**{module.brief}**\n")
    if module.body:
        out.append(module.body + "\n")

    if module.constants:
        out.append("**Constants**\n")
        out.append("| Name | Value |")
        out.append("|---|---|")
        for c in module.constants:
            value = c.value.replace("|", "\\|") or " "
            out.append(f"| `{c.name}` | `{value}` |")
        out.append("")

    if not module.declarations:
        return

    entries = [render_declaration(d) for d in module.declarations]
    out.append("---\n")
    out.append("\n\n---\n\n".join("\n".join(e) for e in entries))
    out.append("\n---\n")


def build_permission_index(modules):
    index = {}
    for module in modules:
        for decl in module.declarations:
            if not decl.doc or not decl.doc.permission:
                continue
            for token in decl.doc.permission_tokens():
                if token in NON_PERMISSION_TOKENS:
                    continue
                index.setdefault(token, []).append(f"`{decl.name}` ({module.filename})")
    return index


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check", action="store_true", help="exit non-zero if the output file would change"
    )
    args = parser.parse_args()

    paths = sorted(glob.glob(os.path.join(SDK_DIR, "*.h")))
    if not paths:
        print(f"no headers found under {SDK_DIR}", file=sys.stderr)
        return 1

    modules = [parse_file(p) for p in paths]

    out = []
    out.append("# BruceOS Core SDK Reference\n")
    out.append(
        "Generated by `tools/gen_sdk_docs.py` from the Doxygen-style comments in "
        "`src/core_sdk/*.h`. Do not edit this file by hand -- edit the header "
        "comments and re-run the script.\n"
    )
    out.append(
        "Every core_sdk function documents the "
        "[`bruce_permission_t`](../src/core_sdk/permission.h) permission(s) it "
        "checks, where the header comments state one. `none` means the header "
        "explicitly says the function is not permission-gated; `built-in only` "
        "means external ELF/JS/WASM apps always get `BRUCE_ERR_PERMISSION` "
        "regardless of granted permissions. No Permissions section at all means "
        "the header comments don't state a permission requirement either "
        "way.\n"
    )

    out.append("## Modules\n")
    for module in modules:
        anchor = github_slug(module.filename)
        brief = module.brief or ""
        out.append(f"- [`{module.filename}`](#{anchor}) -- {brief}")
    out.append("")

    permission_index = build_permission_index(modules)
    if permission_index:
        out.append("## Permissions Reference\n")
        out.append(
            "Functions that explicitly document a `bruce_permission_t` check, "
            "grouped by permission name (see `src/core_sdk/permission.h`).\n"
        )
        for perm in sorted(permission_index):
            out.append(f"### `{perm}`\n")
            for entry in permission_index[perm]:
                out.append(f"- {entry}")
            out.append("")

    out.append("## Header Reference\n")
    for module in modules:
        render_module(module, out)

    rendered = "\n".join(out).rstrip() + "\n"

    if args.check:
        existing = ""
        if os.path.exists(OUTPUT_PATH):
            with open(OUTPUT_PATH, encoding="utf-8") as f:
                existing = f.read()
        if existing != rendered:
            print(f"{OUTPUT_PATH} is out of date; run tools/gen_sdk_docs.py", file=sys.stderr)
            return 1
        print(f"{OUTPUT_PATH} is up to date")
        return 0

    os.makedirs(os.path.dirname(OUTPUT_PATH), exist_ok=True)
    with open(OUTPUT_PATH, "w", encoding="utf-8") as f:
        f.write(rendered)
    print(f"wrote {OUTPUT_PATH} ({len(modules)} headers, "
          f"{sum(len(m.declarations) for m in modules)} declarations)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
