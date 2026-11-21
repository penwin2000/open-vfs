#!/usr/bin/env python3
"""Scan src/ for OPEN_VFS_IMPL / OPEN_VFS_STUB macros, render progress.svg, and pick unclaimed stubs."""

from __future__ import annotations

import argparse
import html
import json
import random
import re
import secrets
import subprocess
import sys
import tempfile
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Set, Tuple

MODULE_ORDER: List[str] = [
    "vfs_core",
    "rpf9",
    "rds5",
    "xvd",
    "render",
    "audio",
    "input",
    "world_vm",
]

MODULE_MILESTONES: Dict[str, Tuple[int, str]] = {
    "vfs_core": (1, "M1: VFS & Containers"),
    "rpf9": (1, "M1: VFS & Containers"),
    "rds5": (1, "M1: VFS & Containers"),
    "xvd": (1, "M1: VFS & Containers"),
    "render": (2, "M2: DX12 Render & Assets"),
    "audio": (3, "M3: Audio, Input & World VM"),
    "input": (3, "M3: Audio, Input & World VM"),
    "world_vm": (3, "M3: Audio, Input & World VM"),
}

SOURCE_EXTS = {".cpp", ".cc", ".cxx", ".hpp", ".h"}
STARTER_LABELS = {"help wanted", "good first issue"}

TAG_RE = re.compile(r"\b(OPEN_VFS_IMPL|OPEN_VFS_STUB)\b")
BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
LINE_COMMENT_RE = re.compile(r"//[^\n]*")
FUNC_NAME_BEFORE_PAREN_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(")

CLAIM_KEY_RE = re.compile(
    r"(?im)^\s*Claim-Key\s*:\s*([A-Za-z0-9_]+)::([A-Za-z_][A-Za-z0-9_]*)\s*$"
)
MODULE_PREFIX_RE = re.compile(r"^\s*\[([A-Za-z0-9_]+)\]")
CONVENTIONAL_PR_MODULE_RE = re.compile(
    r"^\s*(?:feat|fix|chore|refactor|perf|test)\(([^)]+)\)\s*:",
    re.IGNORECASE,
)
IMPLEMENT_TITLE_RE = re.compile(
    r"^\s*\[([A-Za-z0-9_]+)\]\s*Implement\s+([A-Za-z_][A-Za-z0-9_]*)\b",
    re.IGNORECASE,
)
CONVENTIONAL_IMPLEMENT_RE = re.compile(
    r"^\s*(?:feat|fix|chore|refactor|perf|test)\(([A-Za-z0-9_]+)\)\s*:\s*implement\s+([A-Za-z_][A-Za-z0-9_]*)\b",
    re.IGNORECASE,
)
QUALIFIED_SYMBOL_RE = re.compile(r"\b([A-Za-z0-9_]+)::([A-Za-z_][A-Za-z0-9_]*)\b")


@dataclass
class SymbolEntry:
    module: str
    symbol: str
    status: str  # "impl" or "stub"
    file: str
    line: int
    milestone_number: int
    milestone_title: str
    branch_hint: str


def strip_comments_preserve_lines(source: str) -> str:
    """Replace C/C++ comments with spaces while keeping newlines intact."""
    def _blank_block(match: re.Match[str]) -> str:
        return "".join("\n" if ch == "\n" else " " for ch in match.group(0))

    no_blocks = BLOCK_COMMENT_RE.sub(_blank_block, source)
    return LINE_COMMENT_RE.sub(lambda m: " " * len(m.group(0)), no_blocks)


def extract_symbol_name(clean_source: str, tag_end: int) -> str:
    """Extract the function name immediately following an OPEN_VFS_* tag."""
    tail = clean_source[tag_end : tag_end + 400]
    stripped = tail.lstrip()

    # Optional explicit macro form: OPEN_VFS_STUB(symbol_name)
    if stripped.startswith("("):
        explicit = re.match(r"\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)", stripped)
        if explicit:
            return explicit.group(1)

    # Standard prefix attribute form: OPEN_VFS_IMPL ReturnType func_name(...)
    # Stop at the first '{' or ';' so we stay within the signature header.
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


def scan_file(repo_root: Path, module_name: str, file_path: Path) -> List[SymbolEntry]:
    raw_text = file_path.read_text(encoding="utf-8", errors="replace")
    clean_text = strip_comments_preserve_lines(raw_text)
    rel_path = file_path.relative_to(repo_root).as_posix()
    milestone_num, milestone_title = MODULE_MILESTONES.get(
        module_name, (4, "M4: Playable PC Parity")
    )

    entries: List[SymbolEntry] = []
    for match in TAG_RE.finditer(clean_text):
        line_start = clean_text.rfind("\n", 0, match.start()) + 1
        line_prefix = clean_text[line_start : match.start()].lstrip()
        if line_prefix.startswith("#"):
            # Skip #define OPEN_VFS_IMPL / preprocessor lines
            continue

        line_num = clean_text.count("\n", 0, match.start()) + 1
        tag = match.group(1)
        status = "impl" if tag == "OPEN_VFS_IMPL" else "stub"
        symbol = extract_symbol_name(clean_text, match.end())
        branch_hint = f"feat/m{milestone_num}-{module_name}-{symbol}".replace("_", "-")
        entries.append(
            SymbolEntry(
                module=module_name,
                symbol=symbol,
                status=status,
                file=rel_path,
                line=line_num,
                milestone_number=milestone_num,
                milestone_title=milestone_title,
                branch_hint=branch_hint,
            )
        )
    return entries


def scan_repository(repo_root: Path) -> dict:
    src_dir = repo_root / "src"
    discovered_modules: List[str] = []
    if src_dir.is_dir():
        for child in sorted(src_dir.iterdir()):
            if child.is_dir() and not child.name.startswith("."):
                discovered_modules.append(child.name)

    ordered_modules = [m for m in MODULE_ORDER if m in discovered_modules]
    for m in discovered_modules:
        if m not in ordered_modules:
            ordered_modules.append(m)

    modules_report = []
    total_count = 0
    total_impl = 0

    for module_name in ordered_modules:
        mod_dir = src_dir / module_name
        mod_symbols: List[SymbolEntry] = []
        for path in sorted(mod_dir.rglob("*")):
            if path.is_file() and path.suffix.lower() in SOURCE_EXTS:
                mod_symbols.extend(scan_file(repo_root, module_name, path))

        mod_total = len(mod_symbols)
        mod_impl = sum(1 for s in mod_symbols if s.status == "impl")
        mod_stub = mod_total - mod_impl
        mod_pct = round((mod_impl / mod_total) * 100.0, 1) if mod_total else 0.0
        ms_num, ms_title = MODULE_MILESTONES.get(module_name, (4, "M4: Playable PC Parity"))

        total_count += mod_total
        total_impl += mod_impl

        modules_report.append(
            {
                "name": module_name,
                "milestone_number": ms_num,
                "milestone_title": ms_title,
                "total": mod_total,
                "implemented": mod_impl,
                "stubbed": mod_stub,
                "percent": mod_pct,
                "symbols": [asdict(s) for s in mod_symbols],
            }
        )

    total_stub = total_count - total_impl
    overall_pct = round((total_impl / total_count) * 100.0, 1) if total_count else 0.0

    return {
        "summary": {
            "total": total_count,
            "implemented": total_impl,
            "stubbed": total_stub,
            "percent": overall_pct,
        },
        "modules": modules_report,
    }


def _extract_label_names(item: Dict[str, Any]) -> Set[str]:
    raw_labels = item.get("labels") or []
    names: Set[str] = set()
    for lbl in raw_labels:
        if isinstance(lbl, dict):
            name = lbl.get("name")
            if isinstance(name, str) and name.strip():
                names.add(name.strip().lower())
        elif isinstance(lbl, str) and lbl.strip():
            names.add(lbl.strip().lower())
    return names


def _extract_head_ref(item: Dict[str, Any]) -> str:
    head_ref = item.get("headRefName")
    if isinstance(head_ref, str) and head_ref:
        return head_ref
    head_obj = item.get("head")
    if isinstance(head_obj, dict):
        ref = head_obj.get("ref")
        if isinstance(ref, str):
            return ref
    return ""


def _normalize_branch(ref: str) -> str:
    return ref.strip().lower().replace("_", "-")


def analyze_open_work(
    report: dict,
    issues: Sequence[Dict[str, Any]],
    prs: Sequence[Dict[str, Any]],
) -> Tuple[Set[str], Set[Tuple[str, str]]]:
    """Identify busy modules and claimed (module, symbol) pairs from open issues and PRs."""
    mod_lower_map: Dict[str, str] = {}
    known_symbols_by_mod: Dict[str, Set[str]] = {}
    symbol_to_mods: Dict[str, Set[str]] = {}
    branch_hint_to_key: Dict[str, Tuple[str, str]] = {}
    mod_branch_prefixes: List[Tuple[str, str]] = []

    for mod in report["modules"]:
        mod_name = mod["name"]
        mod_lower_map[mod_name.lower()] = mod_name
        ms_num = mod["milestone_number"]
        mod_branch_prefixes.append(
            (f"feat/m{ms_num}-{mod_name.lower().replace('_', '-')}-", mod_name)
        )
        sym_set: Set[str] = set()
        for sym_entry in mod["symbols"]:
            sym = sym_entry["symbol"]
            sym_set.add(sym)
            symbol_to_mods.setdefault(sym, set()).add(mod_name)
            branch_hint_to_key[_normalize_branch(sym_entry["branch_hint"])] = (
                mod_name,
                sym,
            )
        known_symbols_by_mod[mod_name] = sym_set

    # Longest branch prefix first so prefixes cannot shadow one another.
    mod_branch_prefixes.sort(key=lambda pair: len(pair[0]), reverse=True)

    busy_modules: Set[str] = set()
    claimed_keys: Set[Tuple[str, str]] = set()

    def _inspect_item(item: Dict[str, Any], *, is_pr: bool) -> None:
        title = str(item.get("title") or "")
        body = str(item.get("body") or "")
        labels = _extract_label_names(item)
        assignees = item.get("assignees") or []

        # 1. Explicit Claim-Key lines always mark both module busy and symbol claimed.
        claim_matches = list(CLAIM_KEY_RE.finditer(body))
        for cm in claim_matches:
            raw_mod, raw_sym = cm.group(1).lower(), cm.group(2)
            canon_mod = mod_lower_map.get(raw_mod)
            if canon_mod:
                busy_modules.add(canon_mod)
                claimed_keys.add((canon_mod, raw_sym))

        # 2. Any open issue or PR prefixed with [<module>] makes that module busy.
        title_mod: Optional[str] = None
        prefix_match = MODULE_PREFIX_RE.match(title)
        if prefix_match:
            canon_mod = mod_lower_map.get(prefix_match.group(1).lower())
            if canon_mod:
                busy_modules.add(canon_mod)
                title_mod = canon_mod

        # 3. Conventional PR/issue scopes like feat(rpf9): or feat(rds5,ci): mark modules busy.
        conv_match = CONVENTIONAL_PR_MODULE_RE.match(title)
        if conv_match:
            for scope_part in conv_match.group(1).split(","):
                canon_mod = mod_lower_map.get(scope_part.strip().lower())
                if canon_mod:
                    busy_modules.add(canon_mod)
                    if title_mod is None:
                        title_mod = canon_mod

        is_unassigned_starter = (
            (not is_pr)
            and bool(labels & STARTER_LABELS)
            and (not claim_matches)
            and (not assignees)
        )

        # 4. Standard "[<module>] Implement <symbol>" or "feat(<module>): implement <symbol>" titles.
        for pat in (IMPLEMENT_TITLE_RE, CONVENTIONAL_IMPLEMENT_RE):
            imp_match = pat.match(title)
            if imp_match:
                canon_mod = mod_lower_map.get(imp_match.group(1).lower())
                sym = imp_match.group(2)
                if canon_mod:
                    busy_modules.add(canon_mod)
                    if (not is_unassigned_starter) and sym in known_symbols_by_mod.get(
                        canon_mod, set()
                    ):
                        claimed_keys.add((canon_mod, sym))

        # 5. PR branch names (exact branch_hint or feat/m<n>-<module>-* prefix).
        if is_pr:
            head_ref = _extract_head_ref(item)
            if head_ref:
                norm_ref = _normalize_branch(head_ref)
                if norm_ref in branch_hint_to_key:
                    b_mod, b_sym = branch_hint_to_key[norm_ref]
                    busy_modules.add(b_mod)
                    claimed_keys.add((b_mod, b_sym))
                for prefix, mod_name in mod_branch_prefixes:
                    if norm_ref.startswith(prefix):
                        busy_modules.add(mod_name)
                        break

        # Unassigned starter issues labeled help wanted / good first issue without Claim-Key
        # mark their [<module>] busy (spreading new agents to idle modules first), but do not
        # permanently lock out body-mentioned symbols once Tier 2 fallback is needed.
        if is_unassigned_starter:
            return

        # 6. Qualified <module>::<symbol> mentions in title or body.
        combined = f"{title}\n{body}"
        for qm in QUALIFIED_SYMBOL_RE.finditer(combined):
            canon_mod = mod_lower_map.get(qm.group(1).lower())
            sym = qm.group(2)
            if canon_mod and sym in known_symbols_by_mod.get(canon_mod, set()):
                busy_modules.add(canon_mod)
                claimed_keys.add((canon_mod, sym))

        # 7. Whole-word stub symbol mentions in title or body for active claims and PRs.
        for sym, mods in symbol_to_mods.items():
            if not re.search(rf"\b{re.escape(sym)}\b", combined):
                continue
            if title_mod and title_mod in mods:
                busy_modules.add(title_mod)
                claimed_keys.add((title_mod, sym))
            elif len(mods) == 1:
                only_mod = next(iter(mods))
                busy_modules.add(only_mod)
                claimed_keys.add((only_mod, sym))

    for issue in issues:
        if isinstance(issue, dict) and not issue.get("pull_request"):
            _inspect_item(issue, is_pr=False)

    for pr in prs:
        if isinstance(pr, dict):
            _inspect_item(pr, is_pr=True)

    return busy_modules, claimed_keys


def pick_unclaimed_stub(
    report: dict,
    issues: Sequence[Dict[str, Any]],
    prs: Sequence[Dict[str, Any]],
    *,
    milestone: Optional[int] = None,
    seed: Optional[int] = None,
) -> Tuple[Optional[dict], Set[str], Set[Tuple[str, str]]]:
    """Select one unclaimed OPEN_VFS_STUB using idle-module spreading and random tie-breaking."""
    busy_modules, claimed_keys = analyze_open_work(report, issues, prs)

    unclaimed: List[dict] = []
    for mod in report["modules"]:
        for sym in mod["symbols"]:
            if sym["status"] != "stub":
                continue
            if (sym["module"], sym["symbol"]) in claimed_keys:
                continue
            if milestone is not None and sym["milestone_number"] != milestone:
                continue
            unclaimed.append(sym)

    if not unclaimed:
        return None, busy_modules, claimed_keys

    target_ms = min(s["milestone_number"] for s in unclaimed)
    ms_candidates = [s for s in unclaimed if s["milestone_number"] == target_ms]
    idle_candidates = [s for s in ms_candidates if s["module"] not in busy_modules]

    rng = random.Random(seed) if seed is not None else secrets.SystemRandom()

    if idle_candidates:
        idle_mods = sorted({s["module"] for s in idle_candidates})
        chosen_mod = rng.choice(idle_mods)
        mod_pool = [s for s in idle_candidates if s["module"] == chosen_mod]
        chosen = rng.choice(mod_pool)
        selection_tier = "idle_module"
        candidate_count = len(idle_candidates)
    else:
        chosen = rng.choice(ms_candidates)
        selection_tier = "fallback_milestone"
        candidate_count = len(ms_candidates)

    result = {
        "module": chosen["module"],
        "symbol": chosen["symbol"],
        "claim_key": f'{chosen["module"]}::{chosen["symbol"]}',
        "issue_title": f'[{chosen["module"]}] Implement {chosen["symbol"]}',
        "file": chosen["file"],
        "line": chosen["line"],
        "milestone_number": chosen["milestone_number"],
        "milestone_title": chosen["milestone_title"],
        "branch_hint": chosen["branch_hint"],
        "selection_tier": selection_tier,
        "busy_modules": sorted(busy_modules),
        "claimed_keys": sorted(f"{m}::{s}" for m, s in claimed_keys),
        "candidate_count": candidate_count,
    }
    return result, busy_modules, claimed_keys


def fetch_open_work_via_gh(repo: str) -> Tuple[List[Dict[str, Any]], List[Dict[str, Any]]]:
    """Fetch open issues and open PRs using gh issue list / gh pr list (never gh search)."""
    issue_cmd = [
        "gh",
        "issue",
        "list",
        "--repo",
        repo,
        "--state",
        "open",
        "--limit",
        "100",
        "--json",
        "number,title,body,labels,assignees",
    ]
    pr_cmd = [
        "gh",
        "pr",
        "list",
        "--repo",
        repo,
        "--state",
        "open",
        "--limit",
        "100",
        "--json",
        "number,title,body,headRefName,labels",
    ]
    issue_proc = subprocess.run(
        issue_cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if issue_proc.returncode != 0:
        raise RuntimeError(
            f"gh issue list failed (exit {issue_proc.returncode}): {issue_proc.stderr.strip()}"
        )

    pr_proc = subprocess.run(
        pr_cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if pr_proc.returncode != 0:
        raise RuntimeError(
            f"gh pr list failed (exit {pr_proc.returncode}): {pr_proc.stderr.strip()}"
        )

    issues = json.loads(issue_proc.stdout or "[]")
    prs = json.loads(pr_proc.stdout or "[]")
    if not isinstance(issues, list) or not isinstance(prs, list):
        raise RuntimeError("Unexpected non-list JSON returned by GitHub CLI.")
    return issues, prs


def _load_json_list(path_arg: str) -> List[Dict[str, Any]]:
    raw = (
        sys.stdin.read()
        if path_arg == "-"
        else Path(path_arg).read_text(encoding="utf-8")
    )
    parsed = json.loads(raw or "[]")
    if not isinstance(parsed, list):
        raise ValueError(f"Expected JSON array in {path_arg}")
    return parsed


def render_svg(report: dict) -> str:
    """Render a dense emerald-green and slate-grey SVG tile grid grouped by module."""
    summary = report["summary"]
    modules = report["modules"]

    width = 880
    pad_x = 24
    header_h = 84
    row_pad_y = 12
    tile_size = 16
    tile_gap = 4
    tiles_per_row = 24
    label_col_w = 176
    stats_col_w = 112
    grid_x = pad_x + label_col_w
    footer_h = 44

    row_layouts = []
    y_cursor = header_h + 12
    for mod in modules:
        count = max(1, len(mod["symbols"]))
        grid_rows = (count + tiles_per_row - 1) // tiles_per_row
        grid_h = grid_rows * tile_size + (grid_rows - 1) * tile_gap
        row_h = max(36, grid_h + row_pad_y * 2)
        row_layouts.append((mod, y_cursor, row_h, grid_h))
        y_cursor += row_h + 8

    total_height = y_cursor + footer_h
    bar_x = pad_x
    bar_y = 58
    bar_w = width - pad_x * 2
    bar_h = 8
    fill_w = int(round(bar_w * (summary["percent"] / 100.0)))

    lines: List[str] = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{total_height}" '
        f'viewBox="0 0 {width} {total_height}" role="img" '
        f'aria-label="open-vfs subsystem implementation progress">',
        "  <defs>",
        '    <linearGradient id="bgGrad" x1="0%" y1="0%" x2="100%" y2="100%">',
        '      <stop offset="0%" stop-color="#0d1117"/>',
        '      <stop offset="100%" stop-color="#111822"/>',
        "    </linearGradient>",
        '    <linearGradient id="barGrad" x1="0%" y1="0%" x2="100%" y2="0%">',
        '      <stop offset="0%" stop-color="#2ea043"/>',
        '      <stop offset="100%" stop-color="#3fb950"/>',
        "    </linearGradient>",
        "  </defs>",
        f'  <rect x="0.5" y="0.5" width="{width - 1}" height="{total_height - 1}" rx="12" '
        'fill="url(#bgGrad)" stroke="#30363d" stroke-width="1"/>',
        # Header title + summary metric
        f'  <text x="{pad_x}" y="34" fill="#f0f6fc" font-family="ui-sans-serif, -apple-system, BlinkMacSystemFont, Segoe UI, Helvetica, Arial, sans-serif" '
        'font-size="16" font-weight="700">open-vfs Porting Tile Map</text>',
        f'  <text x="{width - pad_x}" y="34" text-anchor="end" fill="#8b949e" '
        'font-family="ui-monospace, SFMono-Regular, Consolas, Liberation Mono, Menlo, monospace" font-size="13">'
        f'<tspan fill="#3fb950" font-weight="700">{summary["implemented"]}</tspan> / {summary["total"]} functions '
        f'({summary["percent"]:.1f}%)</text>',
        # Overall progress bar
        f'  <rect x="{bar_x}" y="{bar_y}" width="{bar_w}" height="{bar_h}" rx="4" fill="#21262d"/>',
    ]
    if fill_w > 0:
        lines.append(
            f'  <rect x="{bar_x}" y="{bar_y}" width="{fill_w}" height="{bar_h}" rx="4" fill="url(#barGrad)"/>'
        )

    # Module rows
    for mod, row_y, row_h, grid_h in row_layouts:
        ms_badge = f'M{mod["milestone_number"]}'
        lines.append(
            f'  <rect x="{pad_x}" y="{row_y}" width="{width - pad_x * 2}" height="{row_h}" '
            'rx="8" fill="#161b22" stroke="#262c36" stroke-width="1"/>'
        )
        # Milestone badge
        badge_y = row_y + (row_h - 18) // 2
        lines.append(
            f'  <rect x="{pad_x + 12}" y="{badge_y}" width="28" height="18" rx="4" '
            'fill="#1f2937" stroke="#30363d" stroke-width="1"/>'
        )
        lines.append(
            f'  <text x="{pad_x + 26}" y="{badge_y + 12.5}" text-anchor="middle" fill="#79c0ff" '
            'font-family="ui-monospace, SFMono-Regular, Consolas, monospace" font-size="10" font-weight="700">'
            f"{ms_badge}</text>"
        )
        # Module name
        text_y = row_y + row_h / 2 + 4.5
        lines.append(
            f'  <text x="{pad_x + 50}" y="{text_y:.1f}" fill="#e6edf3" '
            'font-family="ui-monospace, SFMono-Regular, Consolas, monospace" font-size="13" font-weight="600">'
            f'{html.escape(mod["name"])}</text>'
        )

        # Dense tiles
        tile_start_y = row_y + (row_h - grid_h) // 2
        for idx, sym in enumerate(mod["symbols"]):
            col = idx % tiles_per_row
            row = idx // tiles_per_row
            tx = grid_x + col * (tile_size + tile_gap)
            ty = tile_start_y + row * (tile_size + tile_gap)
            is_impl = sym["status"] == "impl"
            fill = "#2ea043" if is_impl else "#21262d"
            stroke = "#3fb950" if is_impl else "#30363d"
            status_label = "IMPL" if is_impl else "STUB"
            tooltip = html.escape(
                f'{mod["name"]}::{sym["symbol"]} [{status_label}] ({sym["file"]}:{sym["line"]})'
            )
            lines.append(
                f'  <rect x="{tx}" y="{ty}" width="{tile_size}" height="{tile_size}" rx="3" '
                f'fill="{fill}" stroke="{stroke}" stroke-width="1"><title>{tooltip}</title></rect>'
            )

        # Per-module numeric counter
        stats_x = width - pad_x - 14
        lines.append(
            f'  <text x="{stats_x}" y="{text_y:.1f}" text-anchor="end" fill="#8b949e" '
            'font-family="ui-monospace, SFMono-Regular, Consolas, monospace" font-size="12">'
            f'<tspan fill="#e6edf3" font-weight="600">{mod["implemented"]}/{mod["total"]}</tspan> '
            f'({mod["percent"]:.0f}%)</text>'
        )

    # Footer legend
    foot_y = total_height - 22
    lines.extend(
        [
            f'  <rect x="{pad_x}" y="{foot_y - 10}" width="12" height="12" rx="2.5" fill="#2ea043" stroke="#3fb950" stroke-width="1"/>',
            f'  <text x="{pad_x + 18}" y="{foot_y}" fill="#8b949e" font-family="ui-sans-serif, Segoe UI, Arial, sans-serif" font-size="11">OPEN_VFS_IMPL ({summary["implemented"]})</text>',
            f'  <rect x="{pad_x + 150}" y="{foot_y - 10}" width="12" height="12" rx="2.5" fill="#21262d" stroke="#30363d" stroke-width="1"/>',
            f'  <text x="{pad_x + 168}" y="{foot_y}" fill="#8b949e" font-family="ui-sans-serif, Segoe UI, Arial, sans-serif" font-size="11">OPEN_VFS_STUB ({summary["stubbed"]})</text>',
            f'  <text x="{width - pad_x}" y="{foot_y}" text-anchor="end" fill="#6e7681" font-family="ui-monospace, SFMono-Regular, Consolas, monospace" font-size="11">Generated by tools/progress.py</text>',
            "</svg>",
        ]
    )
    return "\n".join(lines) + "\n"


def run_self_tests() -> int:
    """Run synthetic unit tests covering scan_repository and --pick selection rules."""
    with tempfile.TemporaryDirectory(prefix="open_vfs_progress_test_") as tmp:
        repo = Path(tmp)
        synthetic_sources = {
            "src/vfs_core/vfs_core.cpp": (
                "#define OPEN_VFS_IMPL\n"
                "// OPEN_VFS_STUB void ignored_comment()\n"
                "OPEN_VFS_IMPL int ok_core() { return 0; }\n"
                "OPEN_VFS_STUB int open_virtual_stream() { return 0; }\n"
                "OPEN_VFS_STUB int read_aligned_chunk() { return 0; }\n"
            ),
            "src/rpf9/rpf9.cpp": (
                "OPEN_VFS_IMPL int parse_rpf9_header() { return 0; }\n"
                "OPEN_VFS_STUB int parse_rpf9_block_header() { return 0; }\n"
                "OPEN_VFS_STUB int decrypt_rpf9_payload() { return 0; }\n"
            ),
            "src/rds5/rds5.cpp": (
                "OPEN_VFS_STUB int read_rds5_stream_chunk() { return 0; }\n"
                "OPEN_VFS_STUB int decompress_rds5_chunk() { return 0; }\n"
            ),
            "src/xvd/xvd.cpp": (
                "OPEN_VFS_STUB int verify_xvd_hash_tree() { return 0; }\n"
                "OPEN_VFS_STUB int read_xvd_page() { return 0; }\n"
            ),
            "src/render/render.cpp": (
                "OPEN_VFS_STUB int init_dx12_device() { return 0; }\n"
            ),
        }
        for rel, content in synthetic_sources.items():
            path = repo / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")

        report = scan_repository(repo)
        assert report["summary"]["total"] == 11, report["summary"]
        assert report["summary"]["implemented"] == 2, report["summary"]
        assert report["summary"]["stubbed"] == 9, report["summary"]

        # 1. Any open issue prefixed with [<module>] makes that module busy,
        # including seeded starter issues labeled help wanted / good first issue.
        seeded_issues = [
            {
                "number": 13,
                "title": "[xvd] Implement verified XVD v3 header parser and hash verifier",
                "body": "Flip verify_xvd_hash_tree from OPEN_VFS_STUB to OPEN_VFS_IMPL.",
                "labels": [{"name": "help wanted"}],
                "assignees": [],
            },
            {
                "number": 14,
                "title": "[rds5] Implement RFC 1950 zlib chunk decompression",
                "body": "Roadmap starter task.",
                "labels": [{"name": "help wanted"}],
                "assignees": [],
            },
            {
                "number": 15,
                "title": "[rpf9] Wire verified plaintext scanner into CLI",
                "body": "Starter task.",
                "labels": [{"name": "help wanted"}, {"name": "good first issue"}],
                "assignees": [],
            },
        ]
        for seed_val in range(10):
            picked, busy, claimed = pick_unclaimed_stub(
                report, seeded_issues, [], seed=seed_val
            )
            assert picked is not None
            assert busy == {"rpf9", "rds5", "xvd"}, f"Unexpected busy set: {busy}"
            assert not claimed, f"Starter issues without Claim-Key must not claim symbols: {claimed}"
            assert picked["module"] == "vfs_core", f"Expected idle module vfs_core, got {picked}"
            assert picked["selection_tier"] == "idle_module"
            assert picked["claim_key"].startswith("vfs_core::")

        # 2. Open PR prefixed with [vfs_core] or conventional feat(vfs_core): makes vfs_core busy too.
        pr_busy_vfs = [
            {
                "number": 20,
                "title": "[vfs_core] Wire stream helpers",
                "body": "",
                "headRefName": "chore/vfs-helpers",
                "labels": [],
            }
        ]
        picked, busy, _ = pick_unclaimed_stub(report, seeded_issues, pr_busy_vfs, seed=1)
        assert picked is not None
        assert busy == {"vfs_core", "rpf9", "rds5", "xvd"}
        assert picked["selection_tier"] == "fallback_milestone"
        assert picked["milestone_number"] == 1

        # 3. Claim-Key in issue body and PR branch/title exclude claimed stubs cleanly.
        active_issues = seeded_issues + [
            {
                "number": 30,
                "title": "[vfs_core] Implement open_virtual_stream",
                "body": "Claim-Key: vfs_core::open_virtual_stream\n",
                "labels": [],
                "assignees": [],
            }
        ]
        active_prs = [
            {
                "number": 31,
                "title": "feat(rpf9): implement parse_rpf9_block_header",
                "body": "Closes #29\nClaim-Key: rpf9::parse_rpf9_block_header\n",
                "headRefName": "feat/m1-rpf9-parse-rpf9-block-header",
                "labels": [],
            }
        ]
        for seed_val in range(10):
            picked, busy, claimed = pick_unclaimed_stub(
                report, active_issues, active_prs, seed=seed_val
            )
            assert picked is not None
            assert ("vfs_core", "open_virtual_stream") in claimed
            assert ("rpf9", "parse_rpf9_block_header") in claimed
            assert picked["claim_key"] != "vfs_core::open_virtual_stream"
            assert picked["claim_key"] != "rpf9::parse_rpf9_block_header"

        # 4. When all M1 stubs are claimed, --pick advances cleanly to M2 (render).
        all_m1_claims = [
            {
                "number": 100 + idx,
                "title": f"[{mod['name']}] Implement {sym['symbol']}",
                "body": f"Claim-Key: {mod['name']}::{sym['symbol']}\n",
                "labels": [],
                "assignees": [],
            }
            for idx, (mod, sym) in enumerate(
                (m, s)
                for m in report["modules"]
                if m["milestone_number"] == 1
                for s in m["symbols"]
                if s["status"] == "stub"
            )
        ]
        picked, _, _ = pick_unclaimed_stub(report, all_m1_claims, [], seed=0)
        assert picked is not None
        assert picked["module"] == "render"
        assert picked["symbol"] == "init_dx12_device"
        assert picked["milestone_number"] == 2
        assert picked["selection_tier"] == "idle_module"

        # 5. When every stub across all milestones is claimed, returns None.
        all_claims = all_m1_claims + [
            {
                "number": 200,
                "title": "[render] Implement init_dx12_device",
                "body": "Claim-Key: render::init_dx12_device\n",
                "labels": [],
                "assignees": [],
            }
        ]
        picked, _, _ = pick_unclaimed_stub(report, all_claims, [], seed=0)
        assert picked is None, f"Expected None when all stubs claimed, got {picked}"

        # 6. Randomized selection across multiple idle modules produces varied results.
        seen_keys: Set[str] = set()
        for _ in range(40):
            picked, _, _ = pick_unclaimed_stub(report, [], [])
            assert picked is not None
            seen_keys.add(picked["claim_key"])
        assert len(seen_keys) >= 2, f"Expected randomized variation, saw only {seen_keys}"

    print("progress.py self-test passed: scan and --pick selection rules verified.")
    return 0


def main(argv: List[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Scan src/ for OPEN_VFS_IMPL and OPEN_VFS_STUB macros, render progress.svg, or pick an unclaimed stub."
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="Repository root path (default: parent of tools/)",
    )
    parser.add_argument(
        "--scan",
        action="store_true",
        help="Print structured JSON progress status to stdout",
    )
    parser.add_argument(
        "--pick",
        action="store_true",
        help="Pick one unclaimed OPEN_VFS_STUB using idle-module spreading and random tie-breaking",
    )
    parser.add_argument(
        "--from-gh",
        action="store_true",
        help="With --pick, query live open issues and PRs via gh issue list / gh pr list",
    )
    parser.add_argument(
        "--repo",
        default="penwin2000/open-vfs",
        help="GitHub repository for --from-gh (default: penwin2000/open-vfs)",
    )
    parser.add_argument(
        "--issues-json",
        default=None,
        help="Path to JSON array of open issues (or '-' for stdin) for --pick",
    )
    parser.add_argument(
        "--prs-json",
        default=None,
        help="Path to JSON array of open PRs (or '-' for stdin) for --pick",
    )
    parser.add_argument(
        "--milestone",
        type=int,
        default=None,
        help="Restrict --pick to a specific milestone number (default: earliest incomplete milestone)",
    )
    parser.add_argument(
        "--seed",
        type=int,
        default=None,
        help="Optional PRNG seed for deterministic --pick testing",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Output SVG file path (default: <root>/progress.svg when neither --scan nor --pick is requested)",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Run built-in unit tests for scan and --pick and exit",
    )
    args = parser.parse_args(argv)

    if args.self_test:
        return run_self_tests()

    repo_root = args.root.resolve()
    report = scan_repository(repo_root)

    if args.scan:
        print(json.dumps(report, indent=2))

    if args.pick:
        issues: List[Dict[str, Any]] = []
        prs: List[Dict[str, Any]] = []
        live_checked = False
        try:
            if args.from_gh:
                issues, prs = fetch_open_work_via_gh(args.repo)
                live_checked = True
            if args.issues_json is not None:
                issues = _load_json_list(args.issues_json)
                live_checked = True
            if args.prs_json is not None:
                prs = _load_json_list(args.prs_json)
                live_checked = True
        except (OSError, RuntimeError, ValueError) as exc:
            print(f"progress.py --pick error: {exc}", file=sys.stderr)
            return 2

        picked, busy_modules, claimed_keys = pick_unclaimed_stub(
            report,
            issues,
            prs,
            milestone=args.milestone,
            seed=args.seed,
        )
        if picked is None:
            print(
                json.dumps(
                    {
                        "error": "no_unclaimed_stubs",
                        "live_github_checked": live_checked,
                        "busy_modules": sorted(busy_modules),
                        "claimed_keys": sorted(f"{m}::{s}" for m, s in claimed_keys),
                    },
                    indent=2,
                )
            )
            return 1

        picked["live_github_checked"] = live_checked
        print(json.dumps(picked, indent=2))

    if args.output is not None or (not args.scan and not args.pick):
        out_path = args.output if args.output is not None else (repo_root / "progress.svg")
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(render_svg(report), encoding="utf-8")

    return 0


if __name__ == "__main__":
    sys.exit(main())
