#!/usr/bin/env python3
"""Deterministic, zero-secret pull request guard for open-vfs.

Inspects a pull request git diff against a base ref (default: origin/main)
without executing any code from the pull request itself.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Set, Tuple

MAX_ADDED_FILE_BYTES: int = 512 * 1024  # 512 KiB
FULL_BLOB_INSPECT_LIMIT: int = 2 * 1024 * 1024  # 2 MiB
BINARY_PREFIX_BYTES: int = 65536

FORBIDDEN_EXTENSIONS: Set[str] = {
    ".exe",
    ".dll",
    ".lib",
    ".obj",
    ".rpf",
    ".ttpf",
    ".xvd",
    ".pri",
    ".pak",
    ".bin",
    ".zip",
    ".7z",
    ".rar",
}

SIZE_EXEMPT_PATHS: Set[str] = {
    "docs/progress.svg",
    "progress.svg",
}

LOCAL_PATH_SCAN_PREFIXES: Tuple[str, ...] = (
    "src/",
    "include/",
    "app/",
    "tests/",
    "docs/",
)

LOCAL_PATH_EXCLUDED_PATHS: Set[str] = {
    "tools/pr_guard.py",
}

CPP_API_SCAN_PREFIXES: Tuple[str, ...] = (
    "src/",
    "include/",
    "app/",
    "tests/",
)

CONTROL_PATH_PREFIXES: Tuple[str, ...] = (
    ".github/",
    "tools/",
    ".agent/",
)

CONTROL_EXACT_PATHS: Set[str] = {
    "CMakeLists.txt",
    "AGENTS.md",
    ".coderabbit.yaml",
}

FORK_CONTROL_BANNER: str = (
    "SENSITIVE CONTROL FILES MODIFIED BY EXTERNAL FORK "
    "\u2014 MANUAL MAINTAINER AUDIT REQUIRED"
)

LOCAL_PATH_PATTERNS: Tuple[Tuple[str, re.Pattern[str]], ...] = (
    (
        "Windows local drive path prefix (C:\\, D:\\, E:\\)",
        re.compile(r"(?<![A-Za-z0-9_])[CDEcde]:\\"),
    ),
    (
        "Unix/macOS local home path prefix (/Users/, /home/)",
        re.compile(r"(?<![A-Za-z0-9_])/(?:Users|home)/"),
    ),
)

SUSPICIOUS_API_PATTERNS: Tuple[Tuple[str, re.Pattern[str]], ...] = (
    ("system(", re.compile(r"\bsystem\s*\(")),
    ("popen(", re.compile(r"\bpopen\s*\(")),
    ("_popen(", re.compile(r"\b_w?popen\s*\(")),
    ("WinExec", re.compile(r"\bWinExec\b")),
    ("ShellExecute", re.compile(r"\bShellExecute\w*\b")),
    ("CreateProcess", re.compile(r"\bCreateProcess\w*\b")),
    ("WSAStartup", re.compile(r"\bWSAStartup\b")),
    ("WinHttp", re.compile(r"\bWinHttp\w*\b", re.IGNORECASE)),
    ("InternetOpen", re.compile(r"\bInternetOpen\w*\b")),
    ("<winsock", re.compile(r"[<\"]\s*winsock", re.IGNORECASE)),
    ("libcurl", re.compile(r"\blibcurl\w*\b", re.IGNORECASE)),
)

HUNK_HEADER_RE: re.Pattern[str] = re.compile(
    r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,\d+)? @@"
)

CPP_SOURCE_EXTS: Set[str] = {".cpp", ".cc", ".cxx", ".hpp", ".h"}
MACRO_TAG_RE: re.Pattern[str] = re.compile(r"\b(OPEN_VFS_IMPL|OPEN_VFS_STUB)\b")
BLOCK_COMMENT_RE: re.Pattern[str] = re.compile(r"/\*.*?\*/", re.DOTALL)
LINE_COMMENT_RE: re.Pattern[str] = re.compile(r"//[^\n]*")
FUNC_NAME_BEFORE_PAREN_RE: re.Pattern[str] = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(")
CLAIM_KEY_RE: re.Pattern[str] = re.compile(
    r"(?im)^\s*Claim-Key\s*:\s*([A-Za-z0-9_]+)::([A-Za-z_][A-Za-z0-9_]*)\s*$"
)
IMPLEMENT_TITLE_RE: re.Pattern[str] = re.compile(
    r"^\s*\[([A-Za-z0-9_]+)\]\s*Implement\s+([A-Za-z_][A-Za-z0-9_]*)\b",
    re.IGNORECASE,
)
CONVENTIONAL_IMPLEMENT_RE: re.Pattern[str] = re.compile(
    r"^\s*(?:feat|fix|chore|refactor|perf|test)\(([A-Za-z0-9_]+)\)\s*:\s*implement\s+([A-Za-z_][A-Za-z0-9_]*)\b",
    re.IGNORECASE,
)
QUALIFIED_SYMBOL_RE: re.Pattern[str] = re.compile(
    r"\b([A-Za-z0-9_]+)::([A-Za-z_][A-Za-z0-9_]*)\b"
)


@dataclass
class GuardReport:
    base_ref: str
    head_ref: str
    diff_base: str
    is_fork: bool
    changed_file_count: int = 0
    binary_or_dump_errors: List[str] = field(default_factory=list)
    local_path_errors: List[str] = field(default_factory=list)
    suspicious_api_errors: List[str] = field(default_factory=list)
    fork_control_files: List[str] = field(default_factory=list)
    test_coverage_errors: List[str] = field(default_factory=list)
    overlapping_pr_errors: List[str] = field(default_factory=list)
    overlapping_pr_notes: List[str] = field(default_factory=list)

    @property
    def passed(self) -> bool:
        return not (
            self.binary_or_dump_errors
            or self.local_path_errors
            or self.suspicious_api_errors
            or self.fork_control_files
            or self.test_coverage_errors
            or self.overlapping_pr_errors
        )


def run_git(
    repo_root: Path,
    args: Sequence[str],
    *,
    check: bool = True,
) -> subprocess.CompletedProcess[bytes]:
    cmd = ["git", "-c", "core.quotePath=false", *args]
    proc = subprocess.run(
        cmd,
        cwd=str(repo_root),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if check and proc.returncode != 0:
        stderr_text = proc.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"git command failed ({' '.join(cmd)}): {stderr_text}")
    return proc


def resolve_diff_base(repo_root: Path, base_ref: str, head_ref: str) -> str:
    """Resolve the merge-base commit between base_ref and head_ref."""
    run_git(repo_root, ["rev-parse", "--verify", f"{base_ref}^{{commit}}"])
    run_git(repo_root, ["rev-parse", "--verify", f"{head_ref}^{{commit}}"])

    mb = run_git(repo_root, ["merge-base", base_ref, head_ref], check=False)
    if mb.returncode == 0:
        sha = mb.stdout.decode("utf-8", errors="replace").strip()
        if sha:
            return sha
    return (
        run_git(repo_root, ["rev-parse", f"{base_ref}^{{commit}}"])
        .stdout.decode("utf-8", errors="replace")
        .strip()
    )


def get_changed_files(
    repo_root: Path,
    diff_base: str,
    head_ref: str,
) -> List[Tuple[str, str]]:
    """Return [(status, posix_path), ...] from git diff --name-status -z."""
    proc = run_git(
        repo_root,
        [
            "diff",
            "--name-status",
            "-z",
            "--no-renames",
            "--no-ext-diff",
            "--no-color",
            diff_base,
            head_ref,
        ],
    )
    raw = proc.stdout
    if not raw:
        return []

    tokens = raw.split(b"\x00")
    if tokens and tokens[-1] == b"":
        tokens.pop()

    entries: List[Tuple[str, str]] = []
    idx = 0
    while idx < len(tokens):
        status = tokens[idx].decode("utf-8", errors="replace")
        idx += 1
        if not status:
            continue
        if status[0] in ("R", "C"):
            if idx + 1 >= len(tokens):
                break
            old_path = tokens[idx].decode("utf-8", errors="replace")
            new_path = tokens[idx + 1].decode("utf-8", errors="replace")
            idx += 2
            entries.append(("D", old_path))
            entries.append(("A", new_path))
        else:
            if idx >= len(tokens):
                break
            path = tokens[idx].decode("utf-8", errors="replace")
            idx += 1
            entries.append((status, path))
    return entries


def get_git_binary_files(
    repo_root: Path,
    diff_base: str,
    head_ref: str,
) -> Set[str]:
    """Return file paths marked binary ('-\t-\t') by git diff --numstat -z."""
    proc = run_git(
        repo_root,
        [
            "diff",
            "--numstat",
            "-z",
            "--no-renames",
            "--no-ext-diff",
            "--no-color",
            diff_base,
            head_ref,
        ],
    )
    raw = proc.stdout
    if not raw:
        return set()

    binary_paths: Set[str] = set()
    for record in raw.split(b"\x00"):
        if not record:
            continue
        parts = record.split(b"\t", 2)
        if len(parts) == 3 and parts[0] == b"-" and parts[1] == b"-":
            binary_paths.add(parts[2].decode("utf-8", errors="replace"))
    return binary_paths


def get_tree_entry(
    repo_root: Path,
    head_ref: str,
    path: str,
) -> Tuple[str, str, str, int] | None:
    """Return (mode, obj_type, oid, size) for path at head_ref, or None."""
    proc = run_git(repo_root, ["ls-tree", "-l", "-z", head_ref, "--", path])
    raw = proc.stdout
    if not raw:
        return None
    record = raw.split(b"\x00", 1)[0]
    if not record or b"\t" not in record:
        return None
    header_bytes, _ = record.split(b"\t", 1)
    parts = header_bytes.decode("utf-8", errors="replace").split()
    if len(parts) < 4:
        return None
    mode, obj_type, oid, size_str = parts[0], parts[1], parts[2], parts[3]
    size = int(size_str) if size_str.isdigit() else 0
    return mode, obj_type, oid, size


def read_blob_all(repo_root: Path, oid: str) -> bytes:
    return run_git(repo_root, ["cat-file", "blob", oid]).stdout


def read_blob_prefix(repo_root: Path, oid: str, max_bytes: int) -> bytes:
    """Read at most max_bytes from a git blob without buffering huge files."""
    proc = subprocess.Popen(
        ["git", "cat-file", "blob", oid],
        cwd=str(repo_root),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        assert proc.stdout is not None
        data = proc.stdout.read(max_bytes)
    finally:
        if proc.stdout is not None:
            proc.stdout.close()
        if proc.stderr is not None:
            proc.stderr.close()
        if proc.poll() is None:
            proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    return data


def is_binary_bytes(data: bytes, *, full_file: bool) -> Tuple[bool, str]:
    """Return (is_binary, plain_english_reason) for raw file bytes."""
    if not data:
        return False, ""
    if b"\x00" in data:
        return True, "contains NUL bytes (binary/non-text data)"
    if full_file:
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            return True, "is not valid UTF-8 text (binary/non-text data)"
        for ch in text:
            code = ord(ch)
            if code < 0x20 and ch not in ("\t", "\n", "\r", "\f"):
                return True, f"contains non-text control byte 0x{code:02X}"
    return False, ""


def get_added_lines(
    repo_root: Path,
    diff_base: str,
    head_ref: str,
    path: str,
) -> List[Tuple[int, str]]:
    """Return [(new_line_number, line_content), ...] for added lines in path."""
    proc = run_git(
        repo_root,
        [
            "diff",
            "-U0",
            "--no-renames",
            "--no-ext-diff",
            "--no-color",
            diff_base,
            head_ref,
            "--",
            path,
        ],
    )
    diff_text = proc.stdout.decode("utf-8", errors="replace")
    added: List[Tuple[int, str]] = []
    in_hunk = False
    current_line = 0

    for raw_line in diff_text.split("\n"):
        line = raw_line.rstrip("\r")
        if line.startswith("@@ "):
            match = HUNK_HEADER_RE.match(line)
            if match:
                current_line = int(match.group(1))
                in_hunk = True
            continue
        if not in_hunk:
            continue
        if line.startswith("+"):
            added.append((current_line, line[1:]))
            current_line += 1
        elif line.startswith("-"):
            continue
        elif line.startswith("\\"):
            continue
        elif line.startswith(" "):
            current_line += 1
    return added


def is_control_file(path: str) -> bool:
    if path in CONTROL_EXACT_PATHS:
        return True
    if path.endswith("/CMakeLists.txt"):
        return True
    return path.startswith(CONTROL_PATH_PREFIXES)


def _strip_comments_preserve_lines(source: str) -> str:
    def _blank_block(match: re.Match[str]) -> str:
        return "".join("\n" if ch == "\n" else " " for ch in match.group(0))

    no_blocks = BLOCK_COMMENT_RE.sub(_blank_block, source)
    return LINE_COMMENT_RE.sub(lambda m: " " * len(m.group(0)), no_blocks)


def _extract_symbol_name(clean_source: str, tag_end: int) -> str:
    tail = clean_source[tag_end : tag_end + 400]
    stripped = tail.lstrip()

    if stripped.startswith("("):
        explicit = re.match(r"\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)", stripped)
        if explicit:
            return explicit.group(1)

    sig_end = len(tail)
    for stop_ch in ("{", ";"):
        idx = tail.find(stop_ch)
        if idx != -1 and idx < sig_end:
            sig_end = idx
    sig = tail[:sig_end]

    paren_idx = sig.find("(")
    if paren_idx != -1:
        before_paren = sig[: paren_idx + 1]
        matches = list(FUNC_NAME_BEFORE_PAREN_RE.finditer(before_paren))
        if matches:
            return matches[-1].group(1)

    return "unknown_symbol"


def extract_symbol_statuses(source_text: str) -> Dict[str, str]:
    """Return {symbol_name: 'impl' | 'stub'} parsed from C/C++ source text."""
    clean_text = _strip_comments_preserve_lines(source_text)
    statuses: Dict[str, str] = {}
    for match in MACRO_TAG_RE.finditer(clean_text):
        line_start = clean_text.rfind("\n", 0, match.start()) + 1
        line_prefix = clean_text[line_start : match.start()].lstrip()
        if line_prefix.startswith("#"):
            continue
        sym = _extract_symbol_name(clean_text, match.end())
        if sym != "unknown_symbol":
            statuses[sym] = "impl" if match.group(1) == "OPEN_VFS_IMPL" else "stub"
    return statuses


def _read_text_at_ref(repo_root: Path, ref: str, path: str) -> str:
    proc = run_git(repo_root, ["show", f"{ref}:{path}"], check=False)
    if proc.returncode != 0:
        return ""
    return proc.stdout.decode("utf-8", errors="replace")


def _module_from_src_path(path: str) -> Optional[str]:
    parts = path.split("/")
    if len(parts) < 3 or parts[0] != "src":
        return None
    if Path(path).suffix.lower() not in CPP_SOURCE_EXTS:
        return None
    return parts[1]


def find_flipped_symbols(
    repo_root: Path,
    diff_base: str,
    head_ref: str,
    changed_files: Sequence[Tuple[str, str]],
) -> Set[Tuple[str, str]]:
    """Return {(module, symbol)} flipped from OPEN_VFS_STUB to OPEN_VFS_IMPL in this PR."""
    flipped: Set[Tuple[str, str]] = set()
    for status, path in changed_files:
        if status == "D":
            continue
        module = _module_from_src_path(path)
        if not module:
            continue
        before_statuses = extract_symbol_statuses(
            _read_text_at_ref(repo_root, diff_base, path)
        )
        after_statuses = extract_symbol_statuses(
            _read_text_at_ref(repo_root, head_ref, path)
        )
        for sym, after_state in after_statuses.items():
            if before_statuses.get(sym) == "stub" and after_state == "impl":
                flipped.add((module.lower(), sym))
    return flipped


def extract_flipped_symbols_from_patch(
    path: str,
    patch_text: str,
) -> Set[Tuple[str, str]]:
    """Detect OPEN_VFS_STUB -> OPEN_VFS_IMPL flips from a GitHub unified diff patch."""
    module = _module_from_src_path(path)
    if not module or not patch_text:
        return set()

    flipped: Set[Tuple[str, str]] = set()
    before_lines: List[str] = []
    after_lines: List[str] = []

    def _flush_hunk() -> None:
        if not before_lines and not after_lines:
            return
        before_statuses = extract_symbol_statuses("\n".join(before_lines))
        after_statuses = extract_symbol_statuses("\n".join(after_lines))
        for sym, after_state in after_statuses.items():
            if before_statuses.get(sym) == "stub" and after_state == "impl":
                flipped.add((module.lower(), sym))
        before_lines.clear()
        after_lines.clear()

    for raw_line in patch_text.splitlines():
        if raw_line.startswith("@@ "):
            _flush_hunk()
            continue
        if raw_line.startswith("+") and not raw_line.startswith("+++"):
            after_lines.append(raw_line[1:])
        elif raw_line.startswith("-") and not raw_line.startswith("---"):
            before_lines.append(raw_line[1:])
        elif raw_line.startswith(" "):
            text = raw_line[1:]
            before_lines.append(text)
            after_lines.append(text)
    _flush_hunk()
    return flipped


def _extract_pr_head_ref(pr_entry: Dict[str, Any]) -> str:
    head_ref = pr_entry.get("headRefName")
    if isinstance(head_ref, str) and head_ref:
        return head_ref
    head_obj = pr_entry.get("head")
    if isinstance(head_obj, dict):
        ref = head_obj.get("ref")
        if isinstance(ref, str):
            return ref
    return ""


def extract_pr_target_symbols(
    pr_entry: Dict[str, Any],
    candidate_targets: Set[Tuple[str, str]],
) -> Set[Tuple[str, str]]:
    """Extract {(module_lower, symbol)} targeted by another open PR."""
    targets: Set[Tuple[str, str]] = set()
    title = str(pr_entry.get("title") or "")
    body = str(pr_entry.get("body") or "")
    combined = f"{title}\n{body}"

    for cm in CLAIM_KEY_RE.finditer(body):
        targets.add((cm.group(1).lower(), cm.group(2)))

    for pat in (IMPLEMENT_TITLE_RE, CONVENTIONAL_IMPLEMENT_RE):
        im = pat.match(title)
        if im:
            targets.add((im.group(1).lower(), im.group(2)))

    for qm in QUALIFIED_SYMBOL_RE.finditer(combined):
        targets.add((qm.group(1).lower(), qm.group(2)))

    head_ref = _extract_pr_head_ref(pr_entry).strip().lower().replace("_", "-")
    if head_ref:
        for mod_lower, sym in candidate_targets:
            suffix = f"-{mod_lower.replace('_', '-')}-{sym.lower().replace('_', '-')}"
            if head_ref.endswith(suffix):
                targets.add((mod_lower, sym))

    files = pr_entry.get("files") or []
    if isinstance(files, list):
        for file_entry in files:
            if not isinstance(file_entry, dict):
                continue
            filename = str(file_entry.get("filename") or "")
            patch = str(file_entry.get("patch") or "")
            targets.update(extract_flipped_symbols_from_patch(filename, patch))

    return targets


def check_overlapping_prs(
    current_targets: Set[Tuple[str, str]],
    open_prs: Sequence[Dict[str, Any]],
    *,
    current_pr_number: int = 0,
) -> Tuple[List[str], List[str]]:
    """Return (errors, info_notes) when another open PR targets the same symbol."""
    if not current_targets or not open_prs:
        return [], []

    errors: List[str] = []
    notes: List[str] = []

    sorted_prs = sorted(
        (pr for pr in open_prs if isinstance(pr, dict)),
        key=lambda pr: int(pr.get("number") or 0),
    )

    for mod_lower, sym in sorted(current_targets):
        for other_pr in sorted_prs:
            other_num = int(other_pr.get("number") or 0)
            if current_pr_number > 0 and other_num == current_pr_number:
                continue
            other_targets = extract_pr_target_symbols(other_pr, current_targets)
            if (mod_lower, sym) not in other_targets:
                continue

            other_title = str(other_pr.get("title") or "").strip()
            if current_pr_number == 0 or (other_num > 0 and other_num < current_pr_number):
                errors.append(
                    f"{mod_lower}::{sym}: overlaps with earlier open PR #{other_num} "
                    f"({other_title!r}). Coordinate on PR #{other_num} or pick another "
                    "unclaimed OPEN_VFS_STUB via 'python tools/progress.py --pick --from-gh'."
                )
            else:
                notes.append(
                    f"{mod_lower}::{sym}: newer open PR #{other_num} ({other_title!r}) "
                    f"also targets this symbol; PR #{other_num} is blocked in favor of "
                    f"earlier PR #{current_pr_number}."
                )

    return errors, notes


def run_guard(
    repo_root: Path,
    base_ref: str,
    head_ref: str,
    *,
    is_fork: bool,
    open_prs: Sequence[Dict[str, Any]] = (),
    current_pr_number: int = 0,
    current_pr_body: str = "",
) -> GuardReport:
    diff_base = resolve_diff_base(repo_root, base_ref, head_ref)
    changed_files = get_changed_files(repo_root, diff_base, head_ref)
    git_binary_files = get_git_binary_files(repo_root, diff_base, head_ref)

    report = GuardReport(
        base_ref=base_ref,
        head_ref=head_ref,
        diff_base=diff_base,
        is_fork=is_fork,
        changed_file_count=len(changed_files),
    )

    skipped_for_line_scan: Set[str] = set()

    # Check 1: Binary/dump files, forbidden extensions, and 512 KiB size limit
    for status, path in changed_files:
        if status == "D":
            continue

        ext = Path(path).suffix.lower()
        if ext in FORBIDDEN_EXTENSIONS:
            report.binary_or_dump_errors.append(
                f"{path}: forbidden binary or archive file extension '{ext}'. "
                "Remove compiled binaries, archives, and dump files from this PR."
            )

        entry = get_tree_entry(repo_root, head_ref, path)
        if entry is None:
            continue

        mode, obj_type, oid, size = entry
        if obj_type != "blob":
            if mode == "160000":
                report.binary_or_dump_errors.append(
                    f"{path}: git submodules are not permitted in pull requests."
                )
            skipped_for_line_scan.add(path)
            continue

        if path not in SIZE_EXEMPT_PATHS and size > MAX_ADDED_FILE_BYTES:
            report.binary_or_dump_errors.append(
                f"{path}: file size is {size:,} bytes, which exceeds the 512 KiB "
                f"({MAX_ADDED_FILE_BYTES:,} bytes) limit for pull request files."
            )

        if path in git_binary_files:
            report.binary_or_dump_errors.append(
                f"{path}: detected as a binary/non-text blob by git diff. "
                "Only clean-room UTF-8 text source and documentation files are allowed."
            )
            skipped_for_line_scan.add(path)
            continue

        if size > 0:
            if size <= FULL_BLOB_INSPECT_LIMIT:
                blob_bytes = read_blob_all(repo_root, oid)
                is_bin, reason = is_binary_bytes(blob_bytes, full_file=True)
            else:
                blob_bytes = read_blob_prefix(repo_root, oid, BINARY_PREFIX_BYTES)
                is_bin, reason = is_binary_bytes(blob_bytes, full_file=False)
            if is_bin:
                report.binary_or_dump_errors.append(
                    f"{path}: {reason}. Only plain UTF-8 text files are allowed."
                )
                skipped_for_line_scan.add(path)

    # Check 2 & Check 3: Added-line scans for local path leaks and suspicious APIs
    for status, path in changed_files:
        if status == "D" or path in skipped_for_line_scan:
            continue

        scan_local_paths = (
            path not in LOCAL_PATH_EXCLUDED_PATHS
            and path.startswith(LOCAL_PATH_SCAN_PREFIXES)
        )
        scan_cpp_apis = path.startswith(CPP_API_SCAN_PREFIXES)

        if not (scan_local_paths or scan_cpp_apis):
            continue

        added_lines = get_added_lines(repo_root, diff_base, head_ref, path)

        if scan_local_paths:
            for line_no, text in added_lines:
                for label, pattern in LOCAL_PATH_PATTERNS:
                    match = pattern.search(text)
                    if match:
                        report.local_path_errors.append(
                            f"{path}:{line_no}: added line contains {label} "
                            f"(matched '{match.group(0)}'). Replace local machine "
                            "paths with generic placeholders or relative paths."
                        )

        if scan_cpp_apis:
            for line_no, text in added_lines:
                for api_name, pattern in SUSPICIOUS_API_PATTERNS:
                    match = pattern.search(text)
                    if match:
                        report.suspicious_api_errors.append(
                            f"{path}:{line_no}: added C++ line uses disallowed "
                            f"process/network API '{api_name}' (matched "
                            f"'{match.group(0)}'). Remove OS command execution "
                            "and networking calls from clean-room runtime code."
                        )

    # Check 4: Control-file guard on external forks
    if is_fork:
        touched_control = sorted(
            {path for _, path in changed_files if is_control_file(path)}
        )
        report.fork_control_files.extend(touched_control)

    # Check 5: Test coverage reminder when src/ is modified without tests/
    touched_src = sorted({path for _, path in changed_files if path.startswith("src/")})
    touched_tests = sorted(
        {path for _, path in changed_files if path.startswith("tests/")}
    )
    if touched_src and not touched_tests:
        preview = ", ".join(touched_src[:5])
        if len(touched_src) > 5:
            preview += f", ... (+{len(touched_src) - 5} more)"
        report.test_coverage_errors.append(
            f"Files under src/ were modified ({preview}) without any changes "
            "under tests/. Please add or update synthetic-buffer unit tests in "
            "tests/ (for example, tests/test_<module>.cpp) to verify your change."
        )

    # Check 6: Overlapping open pull requests targeting the same OPEN_VFS_STUB symbol
    if open_prs:
        current_targets = find_flipped_symbols(
            repo_root, diff_base, head_ref, changed_files
        )
        if current_pr_body:
            for cm in CLAIM_KEY_RE.finditer(current_pr_body):
                current_targets.add((cm.group(1).lower(), cm.group(2)))
        overlap_errors, overlap_notes = check_overlapping_prs(
            current_targets,
            open_prs,
            current_pr_number=current_pr_number,
        )
        report.overlapping_pr_errors.extend(overlap_errors)
        report.overlapping_pr_notes.extend(overlap_notes)

    return report


def format_report(report: GuardReport) -> str:
    lines: List[str] = [
        "=== open-vfs PR Guard ===",
        f"Base ref : {report.base_ref} (merge-base {report.diff_base[:12]})",
        f"Head ref : {report.head_ref}",
        f"Fork PR  : {'yes (external fork)' if report.is_fork else 'no (same repository)'}",
        f"Files    : {report.changed_file_count} changed",
        "",
    ]

    if report.fork_control_files:
        border = "=" * len(FORK_CONTROL_BANNER)
        lines.extend(
            [
                border,
                FORK_CONTROL_BANNER,
                border,
                "This external fork pull request modifies repository automation, "
                "build scripts, agent instructions, or review configuration:",
            ]
        )
        for path in report.fork_control_files:
            lines.append(f"  - {path}")
        lines.extend(
            [
                "A maintainer must carefully audit every line of these control "
                "files before approving or merging this pull request.",
                "",
            ]
        )

    if report.binary_or_dump_errors:
        lines.append("[FAIL] 1. Binary / dump / oversized files detected:")
        for msg in report.binary_or_dump_errors:
            lines.append(f"  - {msg}")
        lines.append("")

    if report.local_path_errors:
        lines.append("[FAIL] 2. Local machine path leaks detected:")
        for msg in report.local_path_errors:
            lines.append(f"  - {msg}")
        lines.append("")

    if report.suspicious_api_errors:
        lines.append("[FAIL] 3. Suspicious OS/network calls in C++ code detected:")
        for msg in report.suspicious_api_errors:
            lines.append(f"  - {msg}")
        lines.append("")

    if report.test_coverage_errors:
        lines.append("[FAIL] 5. Missing synthetic unit test coverage in tests/:")
        for msg in report.test_coverage_errors:
            lines.append(f"  - {msg}")
        lines.append("")

    if report.overlapping_pr_errors:
        lines.append("[FAIL] 6. Overlapping open pull request(s) detected:")
        for msg in report.overlapping_pr_errors:
            lines.append(f"  - {msg}")
        lines.append("")

    if report.overlapping_pr_notes:
        lines.append("[INFO] Overlapping newer pull request notice:")
        for msg in report.overlapping_pr_notes:
            lines.append(f"  - {msg}")
        lines.append("")

    if report.passed:
        lines.append(
            "RESULT: PASS — All automated clean-room, safety, and coverage checks passed."
        )
    else:
        failed_categories = sum(
            bool(bucket)
            for bucket in (
                report.binary_or_dump_errors,
                report.local_path_errors,
                report.suspicious_api_errors,
                report.fork_control_files,
                report.test_coverage_errors,
                report.overlapping_pr_errors,
            )
        )
        lines.append(
            f"RESULT: FAIL — {failed_categories} guard check(s) failed. "
            "Fix the items listed above and push an updated commit."
        )

    return "\n".join(lines)


def _git_test_cmd(repo: Path, args: Sequence[str]) -> None:
    env = os.environ.copy()
    env.update(
        {
            "GIT_AUTHOR_NAME": "pr-guard-selftest",
            "GIT_AUTHOR_EMAIL": "selftest@example.invalid",
            "GIT_COMMITTER_NAME": "pr-guard-selftest",
            "GIT_COMMITTER_EMAIL": "selftest@example.invalid",
        }
    )
    proc = subprocess.run(
        ["git", "-c", "commit.gpgsign=false", *args],
        cwd=str(repo),
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if proc.returncode != 0:
        raise AssertionError(
            f"self-test git command failed ({' '.join(args)}): "
            f"{proc.stderr.decode('utf-8', errors='replace')}"
        )


def _write_bytes(repo: Path, rel_path: str, data: bytes) -> None:
    target = repo / rel_path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)


def _run_synthetic_case(
    base_files: dict[str, bytes],
    pr_writes: dict[str, bytes],
    *,
    pr_deletes: Sequence[str] = (),
    is_fork: bool = False,
    open_prs: Sequence[Dict[str, Any]] = (),
    current_pr_number: int = 0,
    current_pr_body: str = "",
) -> GuardReport:
    with tempfile.TemporaryDirectory(prefix="open_vfs_pr_guard_test_") as tmp:
        repo = Path(tmp)
        _git_test_cmd(repo, ["init", "-b", "main"])
        for rel_path, content in base_files.items():
            _write_bytes(repo, rel_path, content)
        _git_test_cmd(repo, ["add", "-A"])
        _git_test_cmd(repo, ["commit", "-m", "base"])

        _git_test_cmd(repo, ["checkout", "-b", "pr-branch"])
        for rel_path in pr_deletes:
            target = repo / rel_path
            if target.exists():
                target.unlink()
        for rel_path, content in pr_writes.items():
            _write_bytes(repo, rel_path, content)
        _git_test_cmd(repo, ["add", "-A"])
        _git_test_cmd(repo, ["commit", "-m", "pr change"])

        return run_guard(
            repo,
            "main",
            "HEAD",
            is_fork=is_fork,
            open_prs=open_prs,
            current_pr_number=current_pr_number,
            current_pr_body=current_pr_body,
        )


def run_self_tests() -> int:
    """Run deterministic synthetic-diff self-checks covering all 6 guard rules."""
    base_tree = {
        "README.md": b"# open-vfs\n",
        "AGENTS.md": b"# Agent Manual\n",
        "src/rpf9/rpf9.cpp": b"int ok() { return 0; }\n",
        "tests/test_rpf9.cpp": b"int main() { return 0; }\n",
        "docs/legacy.md": b"Old path C:\\legacy\\folder\n",
    }

    # 1. Clean C++ PR with benign strings ("Usage:\n", URL containing /home/)
    rep = _run_synthetic_case(
        base_tree,
        {
            "src/rpf9/rpf9.cpp": (
                b'const char* usage = "Usage:\\n";\n'
                b'const char* url = "https://example.com/home/docs";\n'
                b"int ok() { return 1; }\n"
            ),
            "tests/test_rpf9.cpp": b"int main() { return 0; }\n// added test\n",
        },
    )
    assert rep.passed, f"Expected clean C++ PR to pass, got:\n{format_report(rep)}"

    # 2. Forbidden binary/archive extensions
    for ext in sorted(FORBIDDEN_EXTENSIONS):
        rep = _run_synthetic_case(
            base_tree,
            {f"docs/sample{ext}": b"plain text pretending to be archive\n"},
        )
        assert rep.binary_or_dump_errors, f"Expected extension {ext} to fail"

    # 3. Binary blob disguised with .txt extension
    rep = _run_synthetic_case(
        base_tree,
        {"docs/blob.txt": b"ELF\x00\x01\x02\xff\xfe"},
    )
    assert rep.binary_or_dump_errors, "Expected binary .txt blob to fail"

    # 4. Oversized file (> 512 KiB) rejected, while docs/progress.svg & progress.svg exempt
    rep = _run_synthetic_case(
        base_tree,
        {"docs/too_big.txt": b"A" * (MAX_ADDED_FILE_BYTES + 1)},
    )
    assert rep.binary_or_dump_errors, "Expected >512 KiB file to fail"

    svg_payload = (
        b'<svg xmlns="http://www.w3.org/2000/svg">'
        + b" " * (MAX_ADDED_FILE_BYTES + 1024)
        + b"</svg>\n"
    )
    rep = _run_synthetic_case(
        base_tree,
        {
            "docs/progress.svg": svg_payload,
            "progress.svg": svg_payload,
        },
    )
    assert rep.passed, f"Expected progress.svg size exemption to pass:\n{format_report(rep)}"

    # 5. Local machine path leaks across all scanned directories + exclusions
    path_samples = [
        ("src/rpf9/rpf9.cpp", b'const char* p = "C:\\\\sample\\\\dir";\n'),
        ("include/types.hpp", b"// D:\\sample\\dir\n"),
        ("app/main.cpp", b'const char* p = "E:\\\\sample\\\\dir";\n'),
        ("tests/test_rpf9.cpp", b'const char* p = "/Users/sample/data";\n'),
        ("docs/guide.md", b"Example: /home/sample/data\n"),
    ]
    for target_file, payload in path_samples:
        writes = {target_file: payload}
        if target_file.startswith("src/"):
            writes["tests/test_rpf9.cpp"] = b"int main() { return 0; }\n// test\n"
        rep = _run_synthetic_case(base_tree, writes)
        assert rep.local_path_errors, f"Expected local path leak in {target_file} to fail"

    # Deleting a pre-existing local path line must pass
    rep = _run_synthetic_case(
        base_tree,
        {"docs/legacy.md": b"Cleaned up documentation.\n"},
    )
    assert rep.passed, f"Expected deleting old path line to pass:\n{format_report(rep)}"

    # tools/pr_guard.py mentioning path prefixes must pass (non-fork)
    rep = _run_synthetic_case(
        base_tree,
        {"tools/pr_guard.py": b'# checks C:\\, D:\\, E:\\, /Users/, /home/\n'},
        is_fork=False,
    )
    assert rep.passed, f"Expected tools/pr_guard.py self-exclusion to pass:\n{format_report(rep)}"

    # 6. Suspicious OS/network APIs strictly inside src/, include/, app/, tests/
    api_samples = [
        ("src/rpf9/rpf9.cpp", b'int f() { return system("echo hi"); }\n'),
        ("src/rpf9/rpf9.cpp", b'void* f() { return popen("ls", "r"); }\n'),
        ("src/rpf9/rpf9.cpp", b'void* f() { return _popen("dir", "r"); }\n'),
        ("app/main.cpp", b'void f() { WinExec("x", 0); }\n'),
        ("app/main.cpp", b"void f() { ShellExecuteW(nullptr, nullptr, nullptr, nullptr, nullptr, 0); }\n"),
        ("include/proc.hpp", b"bool f() { return CreateProcessW(nullptr, nullptr, nullptr, nullptr, false, 0, nullptr, nullptr, nullptr, nullptr); }\n"),
        ("tests/test_rpf9.cpp", b"int f() { return WSAStartup(0, nullptr); }\n"),
        ("src/rpf9/rpf9.cpp", b"void* f() { return WinHttpOpen(nullptr, 0, nullptr, nullptr, 0); }\n"),
        ("src/rpf9/rpf9.cpp", b"void* f() { return InternetOpenA(nullptr, 0, nullptr, nullptr, 0); }\n"),
        ("include/net.hpp", b"#include <winsock2.h>\n"),
        ("tests/test_rpf9.cpp", b"// uses libcurl\n"),
    ]
    for target_file, payload in api_samples:
        writes = {target_file: payload}
        if target_file.startswith("src/"):
            writes["tests/test_rpf9.cpp"] = b"int main() { return 0; }\n// test\n"
        rep = _run_synthetic_case(base_tree, writes)
        assert rep.suspicious_api_errors, f"Expected suspicious API in {target_file} to fail"

    # Docs mentioning OS API names must NOT trigger Check 3
    rep = _run_synthetic_case(
        base_tree,
        {"docs/security.md": b"Never call system(, CreateProcess, or WSAStartup.\n"},
    )
    assert rep.passed, f"Expected docs mention of APIs to pass:\n{format_report(rep)}"

    # 7. External fork control-file guard
    control_samples = [
        ".github/workflows/ci.yml",
        "tools/helper.py",
        ".agent/skills/contribute/SKILL.md",
        "CMakeLists.txt",
        "AGENTS.md",
        ".coderabbit.yaml",
    ]
    for control_path in control_samples:
        fork_rep = _run_synthetic_case(
            base_tree,
            {control_path: b"# modified\n"},
            is_fork=True,
        )
        assert fork_rep.fork_control_files == [control_path], (
            f"Expected fork guard to flag {control_path}"
        )
        rendered = format_report(fork_rep)
        assert FORK_CONTROL_BANNER in rendered, "Expected loud fork audit banner in output"

        trusted_rep = _run_synthetic_case(
            base_tree,
            {control_path: b"# modified\n"},
            is_fork=False,
        )
        assert trusted_rep.passed, (
            f"Expected non-fork PR touching {control_path} to pass:\n{format_report(trusted_rep)}"
        )

    # Deleting a control file from an external fork must also trigger Check 4
    fork_del_rep = _run_synthetic_case(
        base_tree,
        {},
        pr_deletes=("AGENTS.md",),
        is_fork=True,
    )
    assert fork_del_rep.fork_control_files == ["AGENTS.md"], (
        "Expected fork guard to flag deleted control file AGENTS.md"
    )

    # 8. Test coverage reminder when src/ changes without tests/
    rep = _run_synthetic_case(
        base_tree,
        {"src/rpf9/rpf9.cpp": b"int ok() { return 2; }\n"},
    )
    assert rep.test_coverage_errors, "Expected src/ change without tests/ to fail"

    rep = _run_synthetic_case(
        base_tree,
        {"docs/notes.md": b"Documentation update only.\n"},
    )
    assert rep.passed, f"Expected docs-only PR to pass:\n{format_report(rep)}"

    # 9. Overlapping open PR detection (fails check with plain-English error; no PR comments)
    stub_base = dict(base_tree)
    stub_base["src/rpf9/rpf9.cpp"] = (
        b"OPEN_VFS_STUB\n"
        b"int parse_rpf9_block_header() { return 0; }\n"
        b"OPEN_VFS_STUB\n"
        b"int decrypt_rpf9_payload() { return 0; }\n"
    )
    impl_writes = {
        "src/rpf9/rpf9.cpp": (
            b"OPEN_VFS_IMPL\n"
            b"int parse_rpf9_block_header() { return 1; }\n"
            b"OPEN_VFS_STUB\n"
            b"int decrypt_rpf9_payload() { return 0; }\n"
        ),
        "tests/test_rpf9.cpp": b"int main() { return 0; }\n// test block header\n",
    }

    # 9a. Earlier open PR flipping the same symbol in its diff patch -> FAIL
    earlier_patch_pr = [
        {
            "number": 10,
            "title": "feat(rpf9): implement block header",
            "body": "",
            "headRefName": "feat/custom-branch",
            "files": [
                {
                    "filename": "src/rpf9/rpf9.cpp",
                    "patch": (
                        "@@ -1,2 +1,2 @@\n"
                        "-OPEN_VFS_STUB\n"
                        "+OPEN_VFS_IMPL\n"
                        " int parse_rpf9_block_header() { return 1; }\n"
                    ),
                }
            ],
        }
    ]
    rep = _run_synthetic_case(
        stub_base,
        impl_writes,
        open_prs=earlier_patch_pr,
        current_pr_number=20,
    )
    assert not rep.passed and rep.overlapping_pr_errors, (
        "Expected overlapping earlier patch PR to fail PR Guard"
    )
    assert "#10" in rep.overlapping_pr_errors[0]
    assert "rpf9::parse_rpf9_block_header" in rep.overlapping_pr_errors[0]

    # 9b. Earlier open PR claiming the same symbol via Claim-Key -> FAIL
    earlier_claim_pr = [
        {
            "number": 11,
            "title": "work in progress",
            "body": "Claim-Key: rpf9::parse_rpf9_block_header\n",
            "headRefName": "wip/block-hdr",
            "files": [],
        }
    ]
    rep = _run_synthetic_case(
        stub_base,
        impl_writes,
        open_prs=earlier_claim_pr,
        current_pr_number=20,
    )
    assert not rep.passed and rep.overlapping_pr_errors, (
        "Expected overlapping earlier Claim-Key PR to fail PR Guard"
    )
    assert "#11" in rep.overlapping_pr_errors[0]

    # 9c. Earlier open PR matching via branch hint -> FAIL
    earlier_branch_pr = [
        {
            "number": 12,
            "title": "wip",
            "body": "",
            "headRefName": "feat/m1-rpf9-parse-rpf9-block-header",
            "files": [],
        }
    ]
    rep = _run_synthetic_case(
        stub_base,
        impl_writes,
        open_prs=earlier_branch_pr,
        current_pr_number=20,
    )
    assert not rep.passed and rep.overlapping_pr_errors, (
        "Expected overlapping earlier branch-hint PR to fail PR Guard"
    )

    # 9d. Newer open PR (#30 > #20) overlaps -> earlier PR #20 stays PASS with info note
    newer_pr = [
        {
            "number": 30,
            "title": "feat(rpf9): implement parse_rpf9_block_header",
            "body": "Claim-Key: rpf9::parse_rpf9_block_header\n",
            "headRefName": "feat/m1-rpf9-parse-rpf9-block-header",
            "files": [],
        }
    ]
    rep = _run_synthetic_case(
        stub_base,
        impl_writes,
        open_prs=newer_pr,
        current_pr_number=20,
    )
    assert rep.passed, f"Expected earlier PR #20 to stay passing:\n{format_report(rep)}"
    assert rep.overlapping_pr_notes, "Expected informational notice for newer duplicate PR"

    # 9e. Same PR number (#20 == #20) is ignored -> PASS
    self_pr = [
        {
            "number": 20,
            "title": "feat(rpf9): implement parse_rpf9_block_header",
            "body": "Claim-Key: rpf9::parse_rpf9_block_header\n",
            "headRefName": "feat/m1-rpf9-parse-rpf9-block-header",
            "files": [],
        }
    ]
    rep = _run_synthetic_case(
        stub_base,
        impl_writes,
        open_prs=self_pr,
        current_pr_number=20,
    )
    assert rep.passed and not rep.overlapping_pr_notes, (
        f"Expected self PR entry to be ignored:\n{format_report(rep)}"
    )

    # 9f. Earlier open PR targeting a DIFFERENT symbol in the same module -> PASS
    other_sym_pr = [
        {
            "number": 10,
            "title": "feat(rpf9): implement decrypt_rpf9_payload",
            "body": "Claim-Key: rpf9::decrypt_rpf9_payload\n",
            "headRefName": "feat/m1-rpf9-decrypt-rpf9-payload",
            "files": [],
        }
    ]
    rep = _run_synthetic_case(
        stub_base,
        impl_writes,
        open_prs=other_sym_pr,
        current_pr_number=20,
    )
    assert rep.passed, (
        f"Expected distinct symbol in same module to pass:\n{format_report(rep)}"
    )

    print("PR Guard self-test passed: all synthetic diff checks verified.")
    return 0


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Deterministic, zero-secret pull request safety guard for open-vfs."
    )
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path.cwd(),
        help="Git repository root to inspect (default: current working directory).",
    )
    parser.add_argument(
        "--base",
        default="origin/main",
        help="Base git ref to diff against (default: origin/main).",
    )
    parser.add_argument(
        "--head",
        default="HEAD",
        help="Head git ref to inspect (default: HEAD).",
    )
    parser.add_argument(
        "--is-fork",
        action="store_true",
        default=False,
        help="Enable strict external-fork control-file audit guard.",
    )
    parser.add_argument(
        "--open-prs-json",
        type=Path,
        default=None,
        help="Optional path to JSON array of open PRs for overlapping-symbol detection.",
    )
    parser.add_argument(
        "--pr-number",
        type=int,
        default=0,
        help="Current pull request number (0 when running locally).",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        default=False,
        help="Run built-in synthetic-diff unit tests and exit.",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    if args.self_test:
        return run_self_tests()

    open_prs: List[Dict[str, Any]] = []
    if args.open_prs_json is not None:
        try:
            raw = args.open_prs_json.read_text(encoding="utf-8")
            parsed = json.loads(raw or "[]")
            if not isinstance(parsed, list):
                raise ValueError("--open-prs-json must contain a JSON array")
            open_prs = parsed
        except (OSError, ValueError) as exc:
            print(f"PR Guard error reading --open-prs-json: {exc}", file=sys.stderr)
            return 2

    current_pr_body = os.environ.get("OPEN_VFS_PR_BODY", "")
    repo_root = args.repo_root.resolve()
    try:
        report = run_guard(
            repo_root,
            args.base,
            args.head,
            is_fork=args.is_fork,
            open_prs=open_prs,
            current_pr_number=args.pr_number,
            current_pr_body=current_pr_body,
        )
    except RuntimeError as exc:
        print(f"PR Guard error: {exc}", file=sys.stderr)
        return 2

    print(format_report(report))
    return 0 if report.passed else 1


if __name__ == "__main__":
    sys.exit(main())
