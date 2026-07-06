#!/usr/bin/env python3
"""MCP server giving Continue/Qwen agent mode real filesystem access.

Continue's built-in file tools only see the open VS Code workspace, so anything
under ~/Downloads, ~/Desktop, etc. is invisible and Qwen (correctly) refuses to
"locate files". These tools work on ANY absolute or ~-relative path.

list_directory(path)            -> entries in a folder (dirs marked, sizes shown).
find_files(name, root)          -> case-insensitive filename search (like `find -iname`).
read_file(path, max_chars)      -> file contents (text; .docx/.pdf get text extracted).
grep_files(pattern, root, glob) -> case-insensitive content search (like `grep -ri`).

Read-only by design: no write/delete tool, so an agent slip can't damage files.
Stdlib only except FastMCP (already used by the sibling MCP servers).
"""
import fnmatch
import os
import re
import zipfile

from mcp.server.fastmcp import FastMCP

mcp = FastMCP("filesystem")

MAX_CHARS = 20000        # cap returned text so it can't blow the model's context
MAX_HITS = 60            # cap match/entry counts
SKIP_DIRS = {".git", "node_modules", ".venv", "venv", "__pycache__",
             "Library", ".Trash", ".cache"}


def _resolve(path: str) -> str:
    """Expand ~ and env vars; tolerate a stray trailing space in a filename
    (macOS lets files end in a space, which users almost never type)."""
    p = os.path.abspath(os.path.expanduser(os.path.expandvars(path)))
    if os.path.exists(p):
        return p
    # retry a trailing-space variant, then a rstrip variant
    for cand in (path + " ", path.rstrip()):
        c = os.path.abspath(os.path.expanduser(os.path.expandvars(cand)))
        if os.path.exists(c):
            return c
    return p


def _size(n: int) -> str:
    for unit, div in (("GB", 1024**3), ("MB", 1024**2), ("KB", 1024), ("B", 1)):
        if n >= div or unit == "B":
            return f"{n/div:.1f}{unit}" if unit != "B" else f"{n}B"
    return f"{n}B"


@mcp.tool()
def list_directory(path: str = "~") -> str:
    """List the contents of a directory. Accepts absolute or ~-relative paths
    (e.g. '~/Downloads'). Directories are marked with a trailing '/'.

    Args:
        path: Folder to list. Defaults to the home directory.
    """
    d = _resolve(path)
    if not os.path.exists(d):
        return f"[not found] {d}"
    if not os.path.isdir(d):
        return f"[not a directory] {d} (use read_file for a single file)"
    try:
        names = sorted(os.listdir(d))
    except Exception as e:
        return f"[error] {e}"
    if not names:
        return f"{d}\n(empty)"
    out = [d]
    for name in names[:200]:
        full = os.path.join(d, name)
        try:
            if os.path.isdir(full):
                out.append(f"  {name}/")
            else:
                out.append(f"  {name}  [{_size(os.path.getsize(full))}]")
        except OSError:
            out.append(f"  {name}")
    if len(names) > 200:
        out.append(f"  ... (+{len(names)-200} more)")
    return "\n".join(out)


@mcp.tool()
def find_files(name: str, root: str = "~") -> str:
    """Find files by name anywhere under a folder (case-insensitive substring or
    glob, e.g. 'invoice', '*.docx', 'GMH*'). Returns full paths.

    Args:
        name: Filename, substring, or glob pattern to match.
        root: Folder to search under. Defaults to the home directory.
    """
    base = _resolve(root)
    if not os.path.isdir(base):
        return f"[not a directory] {base}"
    pat = name if any(c in name for c in "*?[") else f"*{name}*"
    pat = pat.lower()
    hits = []
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS and not d.startswith(".")]
        for fn in filenames:
            if fnmatch.fnmatch(fn.lower(), pat):
                hits.append(os.path.join(dirpath, fn))
                if len(hits) >= MAX_HITS:
                    return "\n".join(hits) + f"\n... (stopped at {MAX_HITS} matches)"
    return "\n".join(hits) if hits else f"[no files matching '{name}' under {base}]"


def _docx_text(path: str) -> str:
    """Extract visible text from a .docx without python-docx (stdlib zip+regex)."""
    with zipfile.ZipFile(path) as z:
        xml = z.read("word/document.xml").decode("utf-8", "replace")
    xml = re.sub(r"</w:p>", "\n", xml)
    xml = re.sub(r"<[^>]+>", "", xml)
    return re.sub(r"\n{3,}", "\n\n", xml).strip()


@mcp.tool()
def read_file(path: str, max_chars: int = MAX_CHARS) -> str:
    """Read a file's contents. Plain text is returned as-is; .docx has its text
    extracted. Accepts absolute or ~-relative paths.

    Args:
        path: File to read.
        max_chars: Truncate output beyond this many characters.
    """
    f = _resolve(path)
    if not os.path.exists(f):
        return f"[not found] {f}"
    if os.path.isdir(f):
        return f"[is a directory] {f} (use list_directory)"
    cap = min(int(max_chars or MAX_CHARS), 200000)
    try:
        if f.lower().endswith(".docx"):
            text = _docx_text(f)
        else:
            with open(f, "r", encoding="utf-8", errors="replace") as fh:
                text = fh.read(cap + 1)
    except zipfile.BadZipFile:
        return f"[not a valid .docx] {f}"
    except Exception as e:
        return f"[error reading {f}] {e}"
    if len(text) > cap:
        return text[:cap] + f"\n... [truncated at {cap} chars; pass a larger max_chars to read more]"
    return text or "[empty file]"


@mcp.tool()
def grep_files(pattern: str, root: str = "~", glob: str = "*") -> str:
    """Search file CONTENTS for a case-insensitive regex/substring under a folder.
    Returns 'path:line: matching text'.

    Args:
        pattern: Text or regex to search for.
        root: Folder to search under.
        glob: Only search files matching this glob (e.g. '*.md', '*.txt').
    """
    base = _resolve(root)
    if not os.path.isdir(base):
        return f"[not a directory] {base}"
    try:
        rx = re.compile(pattern, re.IGNORECASE)
    except re.error:
        rx = re.compile(re.escape(pattern), re.IGNORECASE)
    hits = []
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS and not d.startswith(".")]
        for fn in filenames:
            if not fnmatch.fnmatch(fn.lower(), glob.lower()):
                continue
            full = os.path.join(dirpath, fn)
            try:
                with open(full, "r", encoding="utf-8", errors="replace") as fh:
                    for i, line in enumerate(fh, 1):
                        if rx.search(line):
                            hits.append(f"{full}:{i}: {line.strip()[:200]}")
                            if len(hits) >= MAX_HITS:
                                return "\n".join(hits) + f"\n... (stopped at {MAX_HITS} matches)"
            except Exception:
                continue
    return "\n".join(hits) if hits else f"[no content matching '{pattern}' in {glob} files under {base}]"


if __name__ == "__main__":
    mcp.run()
