#!/usr/bin/env python3
"""Server-side duplicate-claim bouncer and stale-claim reaper for open-vfs.

Strictly scoped to issues containing an explicit `Claim-Key: <module>::<symbol>`
line and explicitly skips starter issues labeled `help wanted` or `good first issue`.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timedelta, timezone
from typing import Any, Dict, List, Optional, Sequence, Set, Tuple

PROTECTED_LABELS: Set[str] = {"help wanted", "good first issue"}

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


def extract_label_names(item: Dict[str, Any]) -> Set[str]:
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


def is_protected_starter(item: Dict[str, Any]) -> bool:
    return bool(extract_label_names(item) & PROTECTED_LABELS)


def extract_claim_key(body: str) -> Optional[Tuple[str, str]]:
    """Return normalized (module_lower, symbol) from the first Claim-Key line, or None."""
    match = CLAIM_KEY_RE.search(body or "")
    if not match:
        return None
    return match.group(1).lower(), match.group(2)


def _extract_pr_head_ref(pr: Dict[str, Any]) -> str:
    head_ref = pr.get("headRefName")
    if isinstance(head_ref, str) and head_ref:
        return head_ref
    head_obj = pr.get("head")
    if isinstance(head_obj, dict):
        ref = head_obj.get("ref")
        if isinstance(ref, str):
            return ref
    return ""


def pr_references_issue(pr: Dict[str, Any], issue_number: int) -> bool:
    if issue_number <= 0:
        return False
    combined = f"{pr.get('title') or ''}\n{pr.get('body') or ''}"
    return bool(re.search(rf"(?<!\w)#{issue_number}(?!\d)", combined))


def pr_targets_claim_key(pr: Dict[str, Any], claim_key: Tuple[str, str]) -> bool:
    mod_lower, sym = claim_key
    title = str(pr.get("title") or "")
    body = str(pr.get("body") or "")
    combined = f"{title}\n{body}"

    for cm in CLAIM_KEY_RE.finditer(body):
        if cm.group(1).lower() == mod_lower and cm.group(2) == sym:
            return True

    for pat in (IMPLEMENT_TITLE_RE, CONVENTIONAL_IMPLEMENT_RE):
        im = pat.match(title)
        if im and im.group(1).lower() == mod_lower and im.group(2) == sym:
            return True

    for qm in QUALIFIED_SYMBOL_RE.finditer(combined):
        if qm.group(1).lower() == mod_lower and qm.group(2) == sym:
            return True

    head_ref = _extract_pr_head_ref(pr).strip().lower().replace("_", "-")
    if head_ref:
        suffix = f"-{mod_lower.replace('_', '-')}-{sym.lower().replace('_', '-')}"
        if head_ref.endswith(suffix):
            return True

    return False


def issue_claims_key(issue: Dict[str, Any], claim_key: Tuple[str, str]) -> bool:
    """Return True if an open issue actively claims claim_key."""
    if issue.get("pull_request"):
        return False
    mod_lower, sym = claim_key
    body = str(issue.get("body") or "")
    title = str(issue.get("title") or "")

    explicit = extract_claim_key(body)
    if explicit == (mod_lower, sym):
        return True

    if is_protected_starter(issue):
        return False

    im = IMPLEMENT_TITLE_RE.match(title)
    if im and im.group(1).lower() == mod_lower and im.group(2) == sym:
        return True

    return False


def evaluate_claim_issue(
    current_issue: Dict[str, Any],
    open_issues: Sequence[Dict[str, Any]],
    open_prs: Sequence[Dict[str, Any]],
) -> Optional[Dict[str, Any]]:
    """Evaluate a newly opened/edited issue. Return duplicate-close action dict or None."""
    state = str(current_issue.get("state") or "open").lower()
    if state != "open":
        return None

    # Adjustment 1: Protect starter issues unconditionally.
    if is_protected_starter(current_issue):
        return None

    # Adjustment 1: Strictly scope automation to issues with Claim-Key: <module>::<symbol>.
    claim_key = extract_claim_key(str(current_issue.get("body") or ""))
    if claim_key is None:
        return None

    current_num = int(current_issue.get("number") or 0)
    canonical_key = f"{claim_key[0]}::{claim_key[1]}"

    # Check for any earlier open claim issue (#M < #current_num).
    earlier_matches: List[Dict[str, Any]] = []
    for other in open_issues:
        if not isinstance(other, dict) or other.get("pull_request"):
            continue
        other_num = int(other.get("number") or 0)
        if other_num <= 0 or (current_num > 0 and other_num >= current_num):
            continue
        if issue_claims_key(other, claim_key):
            earlier_matches.append(other)

    if earlier_matches:
        winner = min(earlier_matches, key=lambda item: int(item.get("number") or 0))
        return {
            "action": "close_duplicate",
            "issue_number": current_num,
            "claim_key": canonical_key,
            "winner_type": "issue",
            "winner_number": int(winner["number"]),
            "winner_title": str(winner.get("title") or "").strip(),
        }

    # Check for any competing open pull request already targeting this symbol
    # (ignoring PRs that reference current_num itself).
    competing_prs: List[Dict[str, Any]] = []
    for pr in open_prs:
        if not isinstance(pr, dict):
            continue
        if pr_references_issue(pr, current_num):
            continue
        if pr_targets_claim_key(pr, claim_key):
            competing_prs.append(pr)

    if competing_prs:
        winner_pr = min(competing_prs, key=lambda item: int(item.get("number") or 0))
        return {
            "action": "close_duplicate",
            "issue_number": current_num,
            "claim_key": canonical_key,
            "winner_type": "pr",
            "winner_number": int(winner_pr["number"]),
            "winner_title": str(winner_pr.get("title") or "").strip(),
        }

    return None


def _parse_iso_utc(raw: str) -> Optional[datetime]:
    if not raw:
        return None
    text = raw.strip()
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    try:
        dt = datetime.fromisoformat(text)
    except ValueError:
        return None
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt.astimezone(timezone.utc)


def find_stale_claims(
    open_issues: Sequence[Dict[str, Any]],
    open_prs: Sequence[Dict[str, Any]],
    *,
    now_utc: datetime,
    stale_hours: float = 48.0,
) -> List[Dict[str, Any]]:
    """Return Claim-Key issues inactive for >= stale_hours with no linked open PR."""
    stale: List[Dict[str, Any]] = []

    for issue in open_issues:
        if not isinstance(issue, dict) or issue.get("pull_request"):
            continue

        # Adjustment 1: Never touch starter issues labeled help wanted / good first issue.
        if is_protected_starter(issue):
            continue

        # Adjustment 1: Strictly scope reaper to issues containing Claim-Key: <module>::<symbol>.
        claim_key = extract_claim_key(str(issue.get("body") or ""))
        if claim_key is None:
            continue

        issue_num = int(issue.get("number") or 0)
        ts_raw = str(issue.get("updated_at") or issue.get("created_at") or "")
        ts = _parse_iso_utc(ts_raw)
        if ts is None:
            continue

        age_hours = (now_utc - ts).total_seconds() / 3600.0
        if age_hours < stale_hours:
            continue

        has_linked_pr = any(
            isinstance(pr, dict)
            and (pr_references_issue(pr, issue_num) or pr_targets_claim_key(pr, claim_key))
            for pr in open_prs
        )
        if has_linked_pr:
            continue

        stale.append(
            {
                "number": issue_num,
                "title": str(issue.get("title") or "").strip(),
                "claim_key": f"{claim_key[0]}::{claim_key[1]}",
                "age_hours": round(age_hours, 1),
            }
        )

    stale.sort(key=lambda item: item["number"])
    return stale


class GitHubApiClient:
    """Minimal stdlib GitHub REST client using env-provided token (no shell commands)."""

    def __init__(self, repo: str, token: str) -> None:
        self.repo = repo
        self.token = token
        self.base_url = f"https://api.github.com/repos/{repo}"

    def _request(
        self,
        method: str,
        path: str,
        payload: Optional[Dict[str, Any]] = None,
        *,
        allow_422: bool = False,
    ) -> Any:
        url = path if path.startswith("https://") else f"{self.base_url}{path}"
        data = json.dumps(payload).encode("utf-8") if payload is not None else None
        headers = {
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {self.token}",
            "X-GitHub-Api-Version": "2022-11-28",
            "User-Agent": "open-vfs-claim-guard",
        }
        if data is not None:
            headers["Content-Type"] = "application/json"

        req = urllib.request.Request(url, data=data, headers=headers, method=method)
        try:
            with urllib.request.urlopen(req, timeout=30) as resp:
                raw = resp.read().decode("utf-8")
                return json.loads(raw) if raw else None
        except urllib.error.HTTPError as exc:
            if allow_422 and exc.code == 422:
                return None
            err_body = exc.read().decode("utf-8", errors="replace")
            raise RuntimeError(
                f"GitHub API {method} {url} failed with HTTP {exc.code}: {err_body}"
            ) from exc

    def _list_paginated(self, path: str, max_pages: int = 5) -> List[Dict[str, Any]]:
        results: List[Dict[str, Any]] = []
        sep = "&" if "?" in path else "?"
        for page in range(1, max_pages + 1):
            batch = self._request("GET", f"{path}{sep}per_page=100&page={page}")
            if not isinstance(batch, list) or not batch:
                break
            results.extend(item for item in batch if isinstance(item, dict))
            if len(batch) < 100:
                break
        return results

    def list_open_issues(self) -> List[Dict[str, Any]]:
        items = self._list_paginated("/issues?state=open")
        return [item for item in items if not item.get("pull_request")]

    def list_open_prs(self) -> List[Dict[str, Any]]:
        return self._list_paginated("/pulls?state=open")

    def ensure_label(self, name: str, color: str, description: str) -> None:
        self._request(
            "POST",
            "/labels",
            {"name": name, "color": color, "description": description},
            allow_422=True,
        )

    def add_label(self, issue_number: int, label: str) -> None:
        self._request("POST", f"/issues/{issue_number}/labels", {"labels": [label]})

    def comment_issue(self, issue_number: int, body: str) -> None:
        self._request("POST", f"/issues/{issue_number}/comments", {"body": body})

    def close_issue(self, issue_number: int) -> None:
        self._request(
            "PATCH",
            f"/issues/{issue_number}",
            {"state": "closed", "state_reason": "not_planned"},
        )


def run_check_issue_from_env() -> int:
    repo = os.environ["REPO"]
    token = os.environ["GH_TOKEN"]
    issue_number = int(os.environ["ISSUE_NUMBER"])
    issue_title = os.environ.get("ISSUE_TITLE", "")
    issue_body = os.environ.get("ISSUE_BODY", "")
    issue_state = os.environ.get("ISSUE_STATE", "open")
    labels_raw = os.environ.get("ISSUE_LABELS_JSON", "[]")
    labels = json.loads(labels_raw or "[]") or []

    current_issue: Dict[str, Any] = {
        "number": issue_number,
        "title": issue_title,
        "body": issue_body,
        "state": issue_state,
        "labels": labels,
    }

    if is_protected_starter(current_issue):
        print(
            f"Skipping protected starter issue #{issue_number} "
            "(labeled help wanted / good first issue)."
        )
        return 0

    claim_key = extract_claim_key(issue_body)
    if claim_key is None:
        print(
            f"Skipping issue #{issue_number}: no 'Claim-Key: <module>::<symbol>' line found."
        )
        return 0

    client = GitHubApiClient(repo, token)
    open_issues = client.list_open_issues()
    open_prs = client.list_open_prs()

    decision = evaluate_claim_issue(current_issue, open_issues, open_prs)
    if decision is None:
        print(
            f"Claim-Key {claim_key[0]}::{claim_key[1]} on issue #{issue_number} is unique."
        )
        return 0

    winner_ref = (
        f"pull request #{decision['winner_number']}"
        if decision["winner_type"] == "pr"
        else f"issue #{decision['winner_number']}"
    )
    comment_body = (
        f"Closing this duplicate claim for `{decision['claim_key']}` because "
        f"earlier {winner_ref} (`{decision['winner_title']}`) already claims this symbol.\n\n"
        "To pick another unclaimed stub cleanly without conflicts, run:\n"
        "```powershell\n"
        f"python tools/progress.py --pick --from-gh --repo {repo}\n"
        "```\n"
        "(Or inspect `gh issue list --state open` directly if Python is unavailable locally.)"
    )
    client.ensure_label(
        "duplicate-claim",
        "d93f0b",
        "Duplicate OPEN_VFS_STUB claim superseded by an earlier issue or PR",
    )
    client.add_label(issue_number, "duplicate-claim")
    client.comment_issue(issue_number, comment_body)
    client.close_issue(issue_number)
    print(
        f"Closed duplicate claim issue #{issue_number} ({decision['claim_key']}); "
        f"superseded by {winner_ref}."
    )
    return 0


def run_reap_stale_from_env(stale_hours: float, dry_run: bool) -> int:
    repo = os.environ["REPO"]
    token = os.environ["GH_TOKEN"]
    client = GitHubApiClient(repo, token)
    open_issues = client.list_open_issues()
    open_prs = client.list_open_prs()
    now_utc = datetime.now(timezone.utc)

    stale_list = find_stale_claims(
        open_issues,
        open_prs,
        now_utc=now_utc,
        stale_hours=stale_hours,
    )
    if not stale_list:
        print(f"No stale Claim-Key issues found (threshold: {stale_hours:g} hours).")
        return 0

    if not dry_run:
        client.ensure_label(
            "stale-claim",
            "fbca04",
            "Claimed OPEN_VFS_STUB released after inactivity with no linked PR",
        )

    for entry in stale_list:
        num = entry["number"]
        key = entry["claim_key"]
        age = entry["age_hours"]
        if dry_run:
            print(f"[DRY RUN] Would release stale claim #{num} ({key}, {age}h inactive).")
            continue

        comment_body = (
            f"Releasing stale claim for `{key}` after **{age:.1f} hours** of inactivity "
            f"with no linked open pull request (threshold: {stale_hours:g} hours).\n\n"
            "This stub is now back in the unclaimed pool. If you are still working on it, "
            "feel free to open your pull request directly or re-claim it via `Claim-Key`."
        )
        client.add_label(num, "stale-claim")
        client.comment_issue(num, comment_body)
        client.close_issue(num)
        print(f"Released stale claim issue #{num} ({key}, {age}h inactive).")

    return 0


def run_self_tests() -> int:
    """Verify claim-guard and stale-claim-reaper decision logic offline."""
    now = datetime(2026, 11, 21, 18, 0, 0, tzinfo=timezone.utc)
    old_ts = (now - timedelta(hours=72)).strftime("%Y-%m-%dT%H:%M:%SZ")
    fresh_ts = (now - timedelta(hours=12)).strftime("%Y-%m-%dT%H:%M:%SZ")

    seeded_starter = {
        "number": 13,
        "title": "[xvd] Implement verified XVD v3 header parser",
        "body": "Starter roadmap issue.\nClaim-Key: xvd::verify_xvd_hash_tree\n",
        "state": "open",
        "labels": [{"name": "help wanted"}],
        "updated_at": old_ts,
    }
    good_first = {
        "number": 18,
        "title": "[audio] Implement ADAT header parser",
        "body": "Starter roadmap issue without Claim-Key.",
        "state": "open",
        "labels": [{"name": "good first issue"}],
        "updated_at": old_ts,
    }
    regular_no_key = {
        "number": 25,
        "title": "Question about DX12 swapchain",
        "body": "General discussion without Claim-Key.",
        "state": "open",
        "labels": [],
        "updated_at": old_ts,
    }
    claim_first = {
        "number": 42,
        "title": "[vfs_core] Implement open_virtual_stream",
        "body": "Claim-Key: vfs_core::open_virtual_stream\n",
        "state": "open",
        "labels": [],
        "updated_at": fresh_ts,
    }
    claim_second = {
        "number": 43,
        "title": "[vfs_core] Implement open_virtual_stream",
        "body": "Claim-Key: vfs_core::open_virtual_stream\n",
        "state": "open",
        "labels": [],
        "updated_at": fresh_ts,
    }
    stale_claim = {
        "number": 50,
        "title": "[rpf9] Implement parse_rpf9_block_header",
        "body": "Claim-Key: rpf9::parse_rpf9_block_header\n",
        "state": "open",
        "labels": [],
        "updated_at": old_ts,
    }
    stale_with_pr = {
        "number": 51,
        "title": "[rds5] Implement decompress_rds5_chunk",
        "body": "Claim-Key: rds5::decompress_rds5_chunk\n",
        "state": "open",
        "labels": [],
        "updated_at": old_ts,
    }
    open_pr_for_51 = {
        "number": 60,
        "title": "feat(rds5): implement decompress_rds5_chunk",
        "body": "Closes #51\nClaim-Key: rds5::decompress_rds5_chunk\n",
        "headRefName": "feat/m1-rds5-decompress-rds5-chunk",
    }

    open_issues = [
        seeded_starter,
        good_first,
        regular_no_key,
        claim_first,
        claim_second,
        stale_claim,
        stale_with_pr,
    ]
    open_prs = [open_pr_for_51]

    # 1. Starter issues are NEVER closed by claim-guard even if they contain a Claim-Key.
    assert evaluate_claim_issue(seeded_starter, open_issues, open_prs) is None
    assert evaluate_claim_issue(good_first, open_issues, open_prs) is None

    # 2. Issues without Claim-Key are NEVER closed.
    assert evaluate_claim_issue(regular_no_key, open_issues, open_prs) is None

    # 3. Earlier claim #42 stays open; duplicate #43 closes pointing to #42.
    assert evaluate_claim_issue(claim_first, open_issues, open_prs) is None
    dup = evaluate_claim_issue(claim_second, open_issues, open_prs)
    assert dup is not None and dup["winner_type"] == "issue" and dup["winner_number"] == 42, dup

    # 4. Editing #51 after its own PR #60 (Closes #51) is opened must NOT close #51.
    assert evaluate_claim_issue(stale_with_pr, open_issues, open_prs) is None

    # 5. A new claim #65 colliding with existing open PR #60 closes pointing to PR #60.
    claim_vs_pr = {
        "number": 65,
        "title": "[rds5] Implement decompress_rds5_chunk",
        "body": "Claim-Key: rds5::decompress_rds5_chunk\n",
        "state": "open",
        "labels": [],
        "updated_at": fresh_ts,
    }
    dup_pr = evaluate_claim_issue(claim_vs_pr, open_issues, open_prs)
    assert dup_pr is not None and dup_pr["winner_type"] == "pr" and dup_pr["winner_number"] == 60, dup_pr

    # 6. Stale reaper ONLY returns #50 (old Claim-Key with no PR).
    # Starter issues (#13, #18), non-Claim-Key (#25), fresh (#42, #43), and PR-backed (#51) are skipped.
    stale = find_stale_claims(open_issues, open_prs, now_utc=now, stale_hours=48.0)
    assert [s["number"] for s in stale] == [50], f"Expected only #50 stale, got {stale}"

    print("claim_guard.py self-test passed: duplicate guard and stale reaper verified.")
    return 0


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Duplicate-claim guard and stale-claim reaper for open-vfs."
    )
    parser.add_argument(
        "--check-issue",
        action="store_true",
        help="Evaluate the issue from GitHub Actions env vars and close if duplicate.",
    )
    parser.add_argument(
        "--reap-stale",
        action="store_true",
        help="Release stale Claim-Key issues with no linked open PR.",
    )
    parser.add_argument(
        "--stale-hours",
        type=float,
        default=48.0,
        help="Inactivity threshold in hours for --reap-stale (default: 48).",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="With --reap-stale, log stale claims without closing them.",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Run offline unit tests and exit.",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    if args.self_test:
        return run_self_tests()
    if args.check_issue:
        return run_check_issue_from_env()
    if args.reap_stale:
        return run_reap_stale_from_env(stale_hours=args.stale_hours, dry_run=args.dry_run)
    print("Specify one of --self-test, --check-issue, or --reap-stale.", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
