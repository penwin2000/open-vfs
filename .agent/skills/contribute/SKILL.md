---
name: contribute
description: Automatically bootstrap penwin2000/open-vfs inside .open-vfs-repo/, claim an unclaimed OPEN_VFS_STUB symbol under a GitHub Milestone, inspect local container headers read-only without copying data, implement clean-room C++20 code and synthetic CTest coverage, verify Windows Release builds and CI checks, open a cross-repo fork PR or direct PR, and squash-merge after checks pass.
---

# `open-vfs` Autonomous Clean-Room Contributor Skill

Use this skill whenever a user in a local archive workspace asks to:
> `Set up https://github.com/penwin2000/open-vfs and start contributing`

or asks to claim and implement the next pending `OPEN_VFS_STUB` task in `penwin2000/open-vfs`.

---

## Hard Safety & Privacy Rules

- **NEVER** copy, stage, or commit any local dump file (`*.rpf`, `*.ttpf`, `*.xvd`, `*.bin`, `*.dll`, `*.exe`, `*.pri`, `*.xml`, `*.config`, `*.png`, `*.alignmentchunk`, `local_data/`).
- **NEVER** mention any commercial game title, franchise, studio, external reference repository, or local dump filename/path in any code, comment, test, commit, issue, or PR.
- **ALWAYS** isolate all git activity inside `.open-vfs-repo/`.
- **ALWAYS** restrict edits to the single claimed module (`src/<module>/`) and its unit test (`tests/test_<module>.cpp`).
- **NEVER** use `gh pr merge --admin` to bypass checks or branch protection, and **NEVER** attempt to bypass Windows Application Control policies.

---

## Step-by-Step Execution Procedure

### Step 1: Clone or Refresh `.open-vfs-repo/` & Detect Permissions
Check whether `.open-vfs-repo/.git` exists in the current workspace:
- If absent:
  ```powershell
  git clone https://github.com/penwin2000/open-vfs.git .open-vfs-repo
  ```
- If present:
  ```powershell
  git -C .open-vfs-repo checkout main
  git -C .open-vfs-repo pull --ff-only origin main
  ```
Verify GitHub CLI authentication and check whether the signed-in account has write access to `penwin2000/open-vfs`:
```powershell
gh auth status
$perm = gh repo view penwin2000/open-vfs --json viewerPermission --jq .viewerPermission
```
- **Collaborators (`ADMIN`, `MAINTAIN`, or `WRITE`)**: push feature branches directly to `origin`.
- **External contributors (any other permission / no write access)**: fork automatically without re-cloning and configure a `fork` remote for cross-repo pull requests:
  ```powershell
  gh repo fork penwin2000/open-vfs --clone=false
  $ghUser = gh api user --jq .login
  if (-not (git -C .open-vfs-repo remote | Select-String -Quiet '^fork$')) {
    git -C .open-vfs-repo remote add fork "https://github.com/$ghUser/open-vfs.git"
  }
  ```

### Step 2: Select an Unclaimed `OPEN_VFS_STUB` Symbol
Inside `.open-vfs-repo/`, inspect the live progress manifest alongside open GitHub issues and PRs:
```powershell
python tools/progress.py --scan
gh issue list --repo penwin2000/open-vfs --state open --limit 100
gh pr list --repo penwin2000/open-vfs --state open --limit 100
```
Pick one symbol with `"status": "stub"` that is not referenced by any open issue or PR, prioritizing milestones in order:
1. `M1: VFS & Containers` (`vfs_core`, `rpf9`, `rds5`, `xvd`)
2. `M2: DX12 Render & Assets` (`render`)
3. `M3: Audio, Input & World VM` (`audio`, `input`, `world_vm`)
4. `M4: Playable PC Parity` (cross-module parity tasks)

### Step 3: Create the Milestone-Tracked Issue
Open an issue on `penwin2000/open-vfs` assigned to the matching milestone:
```powershell
gh issue create --repo penwin2000/open-vfs `
  --title "[<module>] Implement <symbol>" `
  --milestone "<milestone_title>" `
  --body "Clean-room C++20 implementation of \`<module>::<symbol>\` with synthetic-buffer CTest coverage."
```
If an external contributor lacks permission to set `--milestone`, omit `--milestone` and note the target milestone in the issue body. Capture the created issue number (`#<issue>`).

### Step 4: Create Dedicated Feature Branch
Branch from up-to-date `main` using the required naming scheme `feat/m<num>-<module>-<symbol>` (normalize underscores to hyphens if needed, or match `branch_hint` from `--scan`):
```powershell
git -C .open-vfs-repo checkout -b feat/m<num>-<module>-<symbol>
```

### Step 5: Mandatory Read-Only Local Container Header Inspection
**Required for `rpf9`, `rds5`, and `xvd` parsers:** Before writing or modifying any parser struct, offset calculation, or header reader in `src/rpf9/`, `src/rds5/`, or `src/xvd/`, you **must** inspect matching local container headers (`*.rpf`, `*.ttpf`, `*.rds`, `*.xvd`, `*.alignmentchunk`) in the parent workspace strictly **read-only** first (small leading header slices, e.g. 64–4096 bytes) so struct layouts, field widths, magic constants (`0x52504639`, `0x52445335`, `0x1DD3014E` / `"msft-xvd"` at offset `0x000` or `0x200`), endianness, flags, and 16-byte alignment constraints match real binary bytes.
- Never write into the parent workspace.
- Never copy binary payloads, hashes, string tables, or local filenames/paths into `.open-vfs-repo/`.

### Step 6: Implement Clean-Room C++20 Function & Synthetic Tests
1. Edit `src/<module>/` inside `.open-vfs-repo/`:
   - Implement `<symbol>` cleanly in C++20 with full bounds checking, zero compiler warnings under `/W4 /permissive-`, and `open_vfs::Status` error reporting.
   - Change the function's macro annotation from `OPEN_VFS_STUB` to `OPEN_VFS_IMPL`.
2. Edit `tests/test_<module>.cpp`:
   - Add synthetic in-memory buffer tests covering happy paths, out-of-bounds/truncated buffers, invalid magic/flags, and 16-byte alignment edge cases.

### Step 7: Build `open-vfs.exe` & Verify Tests (Windows CI Rules)
Inside `.open-vfs-repo/`, configure with auto-detected x64 Visual Studio generator and **always** enforce single-process, non-reusable MSBuild nodes on Windows:
```powershell
cmake -B build -A x64
cmake --build build --config Release -- /m:1 /nr:false
```
Confirm:
- `open-vfs.exe` and all test targets compile cleanly with **zero errors and zero compiler warnings**.
- Run the synthetic-buffer test suite locally:
  ```powershell
  ctest --test-dir build -C Release --output-on-failure
  ```
- **Windows Application Control fallback**: If Windows Application Control (WDAC / Smart App Control) blocks executing locally built unsigned test binaries during `ctest`, do **not** attempt to circumvent OS policy. Instead, verify zero compiler warnings locally under `/W4 /permissive-`, open the pull request, and confirm all CTest suites and CLI smoke tests pass in GitHub Actions CI via `gh pr checks --watch`.
- Confirm `<symbol>` appears as `"status": "impl"` via `python tools/progress.py --scan` (when Python is available locally) and confirm `git -C .open-vfs-repo status` lists only intended edits under `src/<module>/` and `tests/test_<module>.cpp`.

### Step 8: Push, Open PR, Watch CI & Merge
Stage, commit, and audit `git -C .open-vfs-repo diff` to guarantee zero mention of local filenames, host paths, commercial game titles, franchises, studios, or external reference repositories:
```powershell
git -C .open-vfs-repo add src/<module>/ tests/test_<module>.cpp
git -C .open-vfs-repo commit -m "feat(<module>): implement <symbol>"
```

#### External Contributors (No Write Access — Cross-Repo Fork PR)
Push to your `fork` remote and open a cross-repo pull request targeting `penwin2000/open-vfs`:
```powershell
git -C .open-vfs-repo push -u fork feat/m<num>-<module>-<symbol>
$prUrl = gh pr create --repo penwin2000/open-vfs `
  --head "${ghUser}:feat/m<num>-<module>-<symbol>" `
  --base main `
  --title "feat(<module>): implement <symbol>" `
  --body "Closes #<issue>`n`nImplements \`<module>::<symbol>\` (clean-room C++20) and adds synthetic-buffer CTest coverage in \`tests/test_<module>.cpp\`."
gh pr checks $prUrl --repo penwin2000/open-vfs --watch
```
Once CI checks are green, hand off the PR for maintainer review and merge.

#### Collaborators (Write Access — Verified Squash Merge)
Push directly to `origin`, open the PR, wait for all CI checks to pass via `gh pr checks --watch`, and squash-merge with `--delete-branch` (**never** use `--admin`):
```powershell
git -C .open-vfs-repo push -u origin feat/m<num>-<module>-<symbol>
$prUrl = gh pr create --repo penwin2000/open-vfs `
  --base main `
  --title "feat(<module>): implement <symbol>" `
  --body "Closes #<issue>`n`nImplements \`<module>::<symbol>\` (clean-room C++20) and adds synthetic-buffer CTest coverage in \`tests/test_<module>.cpp\`."
gh pr checks $prUrl --repo penwin2000/open-vfs --watch
gh pr merge $prUrl --repo penwin2000/open-vfs --squash --delete-branch
git -C .open-vfs-repo checkout main
git -C .open-vfs-repo pull --ff-only origin main
```
Native repository auto-merge is enabled on `penwin2000/open-vfs`; when required checks or approvals are pending, collaborators may optionally queue `gh pr merge $prUrl --repo penwin2000/open-vfs --auto --squash --delete-branch` instead of merging manually, still never using `--admin`.
