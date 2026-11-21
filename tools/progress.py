#!/usr/bin/env python3
"""Scan src/ for OPEN_VFS_IMPL / OPEN_VFS_STUB macros and render progress.svg."""

from __future__ import annotations

import argparse
import html
import json
import re
import sys
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Dict, List, Tuple

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

TAG_RE = re.compile(r"\b(OPEN_VFS_IMPL|OPEN_VFS_STUB)\b")
BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
LINE_COMMENT_RE = re.compile(r"//[^\n]*")
FUNC_NAME_BEFORE_PAREN_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(")


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


def main(argv: List[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Scan src/ for OPEN_VFS_IMPL and OPEN_VFS_STUB macros and render progress.svg."
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
        "--output",
        type=Path,
        default=None,
        help="Output SVG file path (default: <root>/progress.svg when --scan is not exclusively requested)",
    )
    args = parser.parse_args(argv)

    repo_root = args.root.resolve()
    report = scan_repository(repo_root)

    if args.scan:
        print(json.dumps(report, indent=2))

    if args.output is not None or not args.scan:
        out_path = args.output if args.output is not None else (repo_root / "progress.svg")
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(render_svg(report), encoding="utf-8")

    return 0


if __name__ == "__main__":
    sys.exit(main())
