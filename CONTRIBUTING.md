# Contributing to `open-vfs`

Thank you for helping build **`open-vfs`**, a clean-room **C++20** and **DirectX 12** native Windows 11 desktop runtime. This repository is structured so humans and coding agents can implement subsystems concurrently without merge conflicts or legal/privacy exposure.

Before starting, review the core project references:
- **[ARCHITECTURE.md](./ARCHITECTURE.md)** — Subsystem dependency DAG, verified binary layouts, and alignment invariants.
- **[AGENTS.md](./AGENTS.md)** — Autonomous contributor guardrails and Issue -> Branch -> Milestone -> PR operating manual.
- **[.agent/skills/contribute/SKILL.md](./.agent/skills/contribute/SKILL.md)** — Automated end-to-end contribution skill for coding agents.

---

## Quick Start

### Human Developers
1. **Fork & clone** `https://github.com/penwin2000/open-vfs.git` (or clone directly if you have collaborator write access).
2. **Pick an unclaimed stub** via `python tools/progress.py --pick --from-gh --repo penwin2000/open-vfs` (which spreads contributors into idle modules first—treating any open `[<module>]` issue or PR as busy—and breaks ties randomly). Without local Python, run `gh issue list --repo penwin2000/open-vfs --state open --limit 100` and `gh pr list --repo penwin2000/open-vfs --state open --limit 100`, pick an idle module in the earliest incomplete milestone, and select an unclaimed `OPEN_VFS_STUB` in `src/<module>/`.
3. **Open a claim issue & verify immediately:** Include `Claim-Key: <module>::<symbol>` in the issue body, then immediately re-run `gh issue list --repo penwin2000/open-vfs --state open --limit 100` (**never** `gh search issues`, which lags behind freshly opened issues). If an earlier open issue (`#M < #<yours>`) or open PR already claims `<module>::<symbol>`, close yours as a duplicate and pick another stub. Then create a branch named `feat/m<num>-<module>-<symbol>` off up-to-date `main`.
4. **Implement & test** strictly inside `src/<module>/` and `tests/test_<module>.cpp`, flipping `OPEN_VFS_STUB` -> `OPEN_VFS_IMPL`.
5. **Build & verify on Windows** (always pass `-- /m:1 /nr:false` to MSBuild so background worker nodes do not hang):
   ```powershell
   cmake -B build -A x64
   cmake --build build --config Release -- /m:1 /nr:false
   ctest --test-dir build -C Release --output-on-failure
   ```
6. **Open a Pull Request** targeting `main` using [.github/pull_request_template.md](./.github/pull_request_template.md) (including `Closes #<issue>` and `Claim-Key: <module>::<symbol>`) and wait for both **Build, Test & Progress Map** and **PR Guard** checks to pass.

### AI Agents
- Follow **[AGENTS.md](./AGENTS.md)** and **[.agent/skills/contribute/SKILL.md](./.agent/skills/contribute/SKILL.md)** strictly (both carry the identical idle-module spreading, `Claim-Key`, create-then-verify, and fork-PR rules).
- When working inside a local archive directory, keep all git operations isolated inside `.open-vfs-repo/`, inspect local container headers strictly **read-only**, and never copy bytes, filenames, or local paths into the repository.
- External contributor agents without write access must fork `penwin2000/open-vfs`, push feature branches to their `fork` remote, open cross-repo PRs targeting `main`, and hand off once CI checks are green.

---

## Fork-and-PR & Automated Guard Rules

Every pull request and claim issue targeting `main` is automatically verified by four workflows:
1. **`PR Guard`** (`.github/workflows/pr-guard.yml` + `tools/pr_guard.py`) — Zero-secret, read-only diff bouncer that rejects:
   - Binary/non-text blobs, forbidden archive/binary extensions (`.exe`, `.dll`, `.lib`, `.obj`, `.rpf`, `.ttpf`, `.xvd`, `.pri`, `.pak`, `.bin`, `.zip`, `.7z`, `.rar`), or any single added file over **512 KiB**.
   - Local OS absolute path leaks in `src/`, `include/`, `app/`, `tests/`, and `docs/`.
   - Disallowed OS process execution or networking APIs inside `src/`, `include/`, `app/`, and `tests/`.
   - Changes under `src/` that omit corresponding synthetic unit test updates under `tests/`.
   - **Overlapping open PRs:** Fails the check with a plain-English error (without posting comments, preserving read-only fork tokens) if an earlier open PR already implements or claims the same `OPEN_VFS_STUB` symbol.
   - **External fork control-file edits:** External fork PRs touching `.github/`, `tools/`, `.agent/`, `CMakeLists.txt`, `AGENTS.md`, or `.coderabbit.yaml` fail with a mandatory manual maintainer audit banner.
2. **`Build, Test & Progress Map`** (`.github/workflows/progress.yml`) — Compiles `open-vfs.exe` (C++20 `/W4 /permissive-`), runs all synthetic CTest suites, runs `tools/progress.py --self-test`, and smoke-tests the CLI on `windows-latest`.
3. **`Claim Guard`** (`.github/workflows/claim-guard.yml` + `tools/claim_guard.py`) — Strictly scoped to issues containing `Claim-Key: <module>::<symbol>` (explicitly skipping starter issues labeled `help wanted` or `good first issue`). Automatically labels and closes newer duplicate claim issues when an earlier open issue or open PR already claims the same symbol.
4. **`Stale Claims Reaper`** (`.github/workflows/stale-claims.yml` + `tools/claim_guard.py`) — Runs every 6 hours and automatically releases inactive `Claim-Key` issues that have no linked open PR after **48 hours** (explicitly skipping starter issues labeled `help wanted` or `good first issue`).

> **Fork workflow tip:** Keep external fork PRs scoped strictly to `src/<module>/` and `tests/test_<module>.cpp`. Never modify `.github/`, `tools/`, `.agent/`, `CMakeLists.txt`, `AGENTS.md`, or `.coderabbit.yaml` unless specifically coordinated with a maintainer.

---

## Clean-Room & Privacy Rules (Non-Negotiable)

| Rule | Requirement |
| :--- | :--- |
| **No Proprietary or Binary Data** | Never commit, copy, or paste local dump files (`*.rpf`, `*.ttpf`, `*.xvd`, `*.bin`, `*.dll`, `*.exe`, `*.pri`, `*.pak`, `*.zip`, `*.7z`, `*.rar`, `*.alignmentchunk`, or binary blobs). |
| **Zero Forbidden Identifiers** | Never mention any commercial game title, franchise, studio, local dump filename/path, or external reference repository in code, comments, docs, SVGs, commits, issues, or PRs. |
| **Synthetic Tests Only** | All CTest unit tests under `tests/` must construct small in-memory synthetic byte arrays—never read from a local archive directory. |
| **Read-Only Header Inspection** | Local archive headers may be inspected read-only to confirm struct offsets, magic values, flags, and alignment invariants; all implementations must be original clean-room C++20. |
| **Zero Suspicious OS/Network APIs** | Runtime modules and tests must never invoke shell/process execution or networking APIs. |

---

## Module Partitioning & Isolation

Contributors **must** restrict each pull request to a single module and its paired unit test:

| Module | Source Directory | Unit Test File | Milestone |
| :--- | :--- | :--- | :--- |
| `vfs_core` | `src/vfs_core/` | `tests/test_vfs_core.cpp` | `M1: VFS & Containers` |
| `rpf9` | `src/rpf9/` | `tests/test_rpf9.cpp` | `M1: VFS & Containers` |
| `rds5` | `src/rds5/` | `tests/test_rds5.cpp` | `M1: VFS & Containers` |
| `xvd` | `src/xvd/` | `tests/test_xvd.cpp` | `M1: VFS & Containers` |
| `render` | `src/render/` | `tests/test_render.cpp` | `M2: DX12 Render & Assets` |
| `audio` | `src/audio/` | `tests/test_audio.cpp` | `M3: Audio, Input & World VM` |
| `input` | `src/input/` | `tests/test_input.cpp` | `M3: Audio, Input & World VM` |
| `world_vm` | `src/world_vm/` | `tests/test_world_vm.cpp` | `M3: Audio, Input & World VM` |

Do not modify `include/open_vfs/`, `CMakeLists.txt`, or unrelated `src/` directories unless explicitly coordinated in a dedicated architecture issue.

---

## Macro Tagging Convention

Every function definition under `src/<module>/` is tagged immediately before its return type using `<open_vfs/macros.hpp>`:

- `OPEN_VFS_STUB` — Pending implementation (returns `Status::NotImplemented` or a documented safe sentinel).
- `OPEN_VFS_IMPL` — Complete clean-room implementation verified by synthetic-buffer CTest coverage.

When you implement a stub, flip its tag from `OPEN_VFS_STUB` to `OPEN_VFS_IMPL`. CI automatically runs `tools/progress.py` on merge to `main` and publishes the updated `progress.svg` tile map to GitHub Pages.

---

## Issue -> Branch -> Milestone -> PR Walkthrough

<details>
<summary><strong>Step-by-Step CLI Commands (GitHub CLI + CMake + CTest)</strong></summary>

```powershell
# 1. Pick an unclaimed stub (spreads into idle modules first; without local Python use gh issue list / gh pr list directly)
python tools/progress.py --pick --from-gh --repo penwin2000/open-vfs

# 2. Claim the stub with Claim-Key and verify immediately via gh issue list --state open (never gh search issues)
gh issue create --repo penwin2000/open-vfs `
  --title "[rpf9] Implement parse_rpf9_block_header" `
  --milestone "M1: VFS & Containers" `
  --body "Claim-Key: rpf9::parse_rpf9_block_header`n`nClean-room C++20 implementation with synthetic-buffer CTest coverage."
gh issue list --repo penwin2000/open-vfs --state open --limit 100 --json number,title,body
gh pr list --repo penwin2000/open-vfs --state open --limit 100 --json number,title,body,headRefName

# 3. Branch from latest main
git checkout main
git pull --ff-only origin main
git checkout -b feat/m1-rpf9-parse-rpf9-block-header

# 4. Build open-vfs.exe and verify CTest (Windows MSBuild flags required)
cmake -B build -A x64
cmake --build build --config Release -- /m:1 /nr:false
ctest --test-dir build -C Release --output-on-failure

# 5. Confirm macro status, PR Guard checks, and clean git tree
python tools/progress.py --scan
python tools/pr_guard.py --base origin/main --head HEAD
git status

# 6. Push (to 'fork' for external contributors or 'origin' for collaborators) and open a PR
git push -u origin feat/m1-rpf9-parse-rpf9-block-header
gh pr create --repo penwin2000/open-vfs `
  --base main `
  --title "feat(rpf9): implement parse_rpf9_block_header" `
  --body "Closes #<issue_number>`nClaim-Key: rpf9::parse_rpf9_block_header`n`nClean-room C++20 implementation with synthetic-buffer CTest coverage."
```

> **Windows Application Control note:** If local security policy blocks executing unsigned CTest binaries on your machine, confirm zero compiler warnings locally under `/W4 /permissive-`, open the pull request, and verify all CTest suites pass in GitHub Actions via `gh pr checks --watch`.

</details>
