# `open-vfs` Agent Operating Manual

This repository builds **`open-vfs.exe`**, a clean-room **C++20** and **DirectX 12** native Windows 11 desktop runtime for user-supplied local console archive directories.

When a contributor opens their local data directory in any AI coding harness (such as Claude Code, Codex, Cursor, Windsurf, Gemini CLI, GitHub Copilot, or Antigravity) and says:

> `Set up https://github.com/penwin2000/open-vfs and start contributing`

follow the automated **Pick -> Claim & Verify -> Branch -> PR** workflow below (synchronized with `.agent/skills/contribute/SKILL.md`).

---

## Non-Negotiable Privacy & Clean-Room Guardrails

1. **Never copy or commit proprietary files.** The parent workspace holds raw local archives (`*.rpf`, `*.ttpf`, `*.xvd`, `*.bin`, `*.dll`, `*.exe`, `*.pri`, `*.xml`, `*.config`, `*.png`, `*.alignmentchunk`). Keep all repository operations strictly inside `.open-vfs-repo/`.
2. **Read-only header inspection only.** You may inspect small binary header slices (e.g., first 64–4096 bytes) of local archives in the parent directory strictly read-only to confirm field offsets, magic constants, flags, and 16-byte alignment invariants. Never copy raw bytes, payloads, hash tables, or strings into the repository.
3. **Zero forbidden identifiers.** Never mention any commercial game title, franchise, studio, external reference repository, or local dump filename/path in any source file, comment, test, SVG, commit message, GitHub issue, or pull request.
4. **Module isolation.** Touch **only** the target module directory (`src/<module>/`) and its corresponding unit test (`tests/test_<module>.cpp`) so concurrent contributors never collide. External fork PRs must never modify `.github/`, `tools/`, `.agent/`, `CMakeLists.txt`, `AGENTS.md`, or `.coderabbit.yaml`.
5. **Synthetic-buffer tests only.** Every unit test must construct small in-memory synthetic byte arrays—never read from the user's local archive directory.
6. **Never bypass safeguards.** Never use `gh pr merge --admin` to bypass checks or branch protection, and never attempt to bypass Windows Application Control policies.

---

## Automated Contributor Workflow

Execute these 8 steps in order:

### 1. Bootstrap `.open-vfs-repo/` & Detect Permissions
- If `.open-vfs-repo/.git` does not exist in the workspace, clone `https://github.com/penwin2000/open-vfs.git` into `.open-vfs-repo/`.
- Otherwise, `git -C .open-vfs-repo checkout main` and `git -C .open-vfs-repo pull --ff-only origin main`.
- Run all subsequent repository commands with `.open-vfs-repo/` as the working directory.
- Verify GitHub CLI authentication and check repository permissions:
  ```powershell
  gh auth status
  $perm = gh repo view penwin2000/open-vfs --json viewerPermission --jq .viewerPermission
  ```
  - **Collaborators (`ADMIN`, `MAINTAIN`, or `WRITE`)**: push feature branches directly to `origin`.
  - **External contributors (any other permission)**: fork automatically without re-cloning and add a `fork` remote:
    ```powershell
    gh repo fork penwin2000/open-vfs --clone=false
    $ghUser = gh api user --jq .login
    if (-not (git remote | Select-String -Quiet '^fork$')) {
      git remote add fork "https://github.com/$ghUser/open-vfs.git"
    }
    ```

### 2. Pick an Unclaimed `OPEN_VFS_STUB` (Idle-Module Spreading)
Prefer the built-in `--pick` selector when Python is available locally:
```powershell
python tools/progress.py --pick --from-gh --repo penwin2000/open-vfs
```
`--pick` automatically queries live open issues and PRs, treats **any** open issue or PR prefixed with `[<module>]` as making that module busy so concurrent contributors spread into truly idle modules first, prioritizes the earliest incomplete milestone (`M1` -> `M2` -> `M3` -> `M4`), and breaks ties randomly to prevent simultaneous-start collisions.

**Fallback without local Python:** Query open issues and PRs directly with `gh` (never `gh search issues`):
```powershell
gh issue list --repo penwin2000/open-vfs --state open --limit 100
gh pr list --repo penwin2000/open-vfs --state open --limit 100
```
Inspect `src/<module>/` for `OPEN_VFS_STUB` declarations, treat any module with an open `[<module>]` issue or PR as busy, choose a random unclaimed stub from an idle module in the earliest incomplete milestone, and fall back to any unclaimed stub in that milestone only if every module is busy.

| Milestone | Exact GitHub Milestone Title | Modules |
| :--- | :--- | :--- |
| `M1` | `M1: VFS & Containers` | `vfs_core`, `rpf9`, `rds5`, `xvd` |
| `M2` | `M2: DX12 Render & Assets` | `render` |
| `M3` | `M3: Audio, Input & World VM` | `audio`, `input`, `world_vm` |
| `M4` | `M4: Playable PC Parity` | Integration & full parity tasks |

### 3. Open Claim Issue & Run Immediate Create-Then-Verify Tie-Breaker
Open a milestone-assigned issue containing the exact machine-parseable `Claim-Key: <module>::<symbol>` line in its body:
```powershell
gh issue create --repo penwin2000/open-vfs `
  --title "[<module>] Implement <symbol>" `
  --milestone "<Exact GitHub Milestone Title>" `
  --body "Claim-Key: <module>::<symbol>`n`nClean-room C++20 implementation of \`<module>::<symbol>\` with synthetic-buffer CTest coverage."
```
If an external contributor lacks permission to set `--milestone`, omit `--milestone` and note the target milestone in the issue body. Capture the created issue number `#<issue>`.

**Mandatory immediate verification (atomic monotonic tie-breaker):** Immediately after creating `#<issue>`, re-list open issues and PRs using `gh issue list --state open` (**never** use `gh search issues`, which is eventually consistent and can miss claims created seconds earlier):
```powershell
gh issue list --repo penwin2000/open-vfs --state open --limit 100 --json number,title,body
gh pr list --repo penwin2000/open-vfs --state open --limit 100 --json number,title,body,headRefName
```
- If any **lower-numbered** open issue (`#M < #<issue>`) or any open pull request already claims `<module>::<symbol>`, you lost the race. Immediately close `#<issue>` and return to Step 2 to pick another stub:
  ```powershell
  gh issue close <issue> --repo penwin2000/open-vfs --reason "not planned" `
    --comment "Duplicate claim superseded by #<winner>; selecting another unclaimed stub."
  ```
- Server-side `claim-guard.yml` and `stale-claims.yml` enforce the same `Claim-Key` rules (auto-closing duplicate `Claim-Key` issues and releasing inactive `Claim-Key` issues with no open PR after 48 hours) while explicitly skipping starter issues labeled `help wanted` or `good first issue`.

### 4. Create a Dedicated Branch
Create a branch off up-to-date `main` matching `feat/m<num>-<module>-<symbol>` (normalize underscores to hyphens, matching `branch_hint` from `--pick`/`--scan`):
```powershell
git checkout -b feat/m<num>-<module>-<symbol>
```

### 5. Read-Only Local Header Inspection (When Applicable)
For container parsers (`vfs_core`, `rpf9`, `rds5`, `xvd`) or asset decoders (`render`, `audio`, `world_vm`), inspect matching archive headers (`*.rpf`, `*.ttpf`, `*.rds`, `*.xvd`, `*.alignmentchunk`) in the parent local data folder **read-only** first to validate magic values (`0x52504639`, `0x52445335`, `0x1DD3014E` / `"msft-xvd"` at offset `0x000` or `0x200`), struct field widths, endianness, flags, and 16-byte alignment constraints. Never write into the parent workspace or copy bytes or local filenames into `.open-vfs-repo/`.

### 6. Implement Clean-Room C++20 & Synthetic Unit Tests
- Implement `<symbol>` in `src/<module>/` using clean, bounds-checked C++20 (`std::span`, `open_vfs::Status`, zero compiler warnings under `/W4 /permissive-`, zero raw pointer overflows).
- Flip its macro tag from `OPEN_VFS_STUB` to `OPEN_VFS_IMPL`.
- Add thorough synthetic-buffer unit tests (happy paths, malformed/corrupt inputs, invalid magic/flags, 16-byte alignment/bounds errors) in `tests/test_<module>.cpp`.

### 7. Build & Verify Locally
```powershell
cmake -B build -A x64
cmake --build build --config Release -- /m:1 /nr:false
ctest --test-dir build -C Release --output-on-failure
```
> **Note (Windows MSBuild):** Always pass `-- /m:1 /nr:false` so MSBuild does not leave persistent background worker nodes hanging in automated or sandboxed shells.
> **Windows Application Control fallback:** If WDAC / Smart App Control blocks executing locally built unsigned test binaries during `ctest`, do **not** attempt to circumvent OS policy. Verify zero compiler warnings locally under `/W4 /permissive-`, open the PR, and confirm all CTest suites and CLI smoke tests pass in GitHub Actions CI via `gh pr checks --watch`.

When Python is available locally, confirm `<symbol>` shows as `"status": "impl"` and run the local PR Guard check:
```powershell
python tools/progress.py --scan
python tools/pr_guard.py --base origin/main --head HEAD
```
Confirm `git status` shows **only** intentional source/test edits under `src/<module>/` and `tests/test_<module>.cpp` (zero ignored or local dump artifacts).

### 8. Commit, Push, Watch CI & Open Linked Pull Request
Stage, commit, and audit `git diff` to guarantee **zero** mention of local filenames, directory names, commercial titles, franchises, studios, or external reference repositories:
```powershell
git add src/<module>/ tests/test_<module>.cpp
git commit -m "feat(<module>): implement <symbol>"
```
Include `Closes #<issue>` and `Claim-Key: <module>::<symbol>` in the PR body so `PR Guard` can verify there are no overlapping open PRs (failing the check with a plain-English error if an earlier PR already targets the same symbol).

#### External Contributors (No Write Access — Cross-Repo Fork PR)
```powershell
git push -u fork feat/m<num>-<module>-<symbol>
$prUrl = gh pr create --repo penwin2000/open-vfs `
  --head "${ghUser}:feat/m<num>-<module>-<symbol>" `
  --base main `
  --title "feat(<module>): implement <symbol>" `
  --body "Closes #<issue>`nClaim-Key: <module>::<symbol>`n`nClean-room C++20 implementation of \`<module>::<symbol>\` with synthetic-buffer CTest coverage in \`tests/test_<module>.cpp\`."
gh pr checks $prUrl --repo penwin2000/open-vfs --watch
```
Once CI checks are green, hand off the PR for maintainer review and merge.

#### Collaborators (Write Access — Verified Squash Merge)
```powershell
git push -u origin feat/m<num>-<module>-<symbol>
$prUrl = gh pr create --repo penwin2000/open-vfs `
  --base main `
  --title "feat(<module>): implement <symbol>" `
  --body "Closes #<issue>`nClaim-Key: <module>::<symbol>`n`nClean-room C++20 implementation of \`<module>::<symbol>\` with synthetic-buffer CTest coverage in \`tests/test_<module>.cpp\`."
gh pr checks $prUrl --repo penwin2000/open-vfs --watch
gh pr merge $prUrl --repo penwin2000/open-vfs --squash --delete-branch
git checkout main
git pull --ff-only origin main
```
Never use `gh pr merge --admin`. Collaborators may optionally queue `--auto --squash --delete-branch` when required checks are pending.
