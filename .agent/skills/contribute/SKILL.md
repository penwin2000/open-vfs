---
name: contribute
description: Automatically bootstrap penwin2000/open-vfs inside .open-vfs-repo/, claim an unclaimed OPEN_VFS_STUB symbol under a GitHub Milestone, inspect local archive headers read-only without copying data, implement clean-room C++20 code and synthetic CTest coverage, verify open-vfs.exe builds and tests pass, and open a linked pull request.
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

---

## Step-by-Step Execution Procedure

### Step 1: Clone or Refresh `.open-vfs-repo/`
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
Verify GitHub CLI authentication (`gh auth status`) before creating issues or PRs.

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
Capture the created issue number (`#<issue>`).

### Step 4: Create Dedicated Feature Branch
Branch from `main` using the required naming scheme `feat/m<num>-<module>-<symbol>` (normalize underscores to hyphens if needed, or match `branch_hint` from `--scan`):
```powershell
git -C .open-vfs-repo checkout -b feat/m<num>-<module>-<symbol>
```

### Step 5: Read-Only Local Header Inspection
If the workspace contains matching archive files (`.rpf`, `.ttpf`, `.xvd`, `.alignmentchunk`, etc.), you may inspect small header slices **read-only** to verify endianness, magic constants (`0x52504639`, `0x52445335`, `0x1DD3014E`), entry sizes, flags, and 16-byte alignment constraints.
- Never write into the parent workspace.
- Never copy binary snippets, hashes, or filenames into `.open-vfs-repo/`.

### Step 6: Implement Clean-Room C++20 Function & Synthetic Tests
1. Edit `src/<module>/` inside `.open-vfs-repo/`:
   - Implement `<symbol>` cleanly in C++20 with full bounds checking and `open_vfs::Status` error reporting.
   - Change the function's macro annotation from `OPEN_VFS_STUB` to `OPEN_VFS_IMPL`.
2. Edit `tests/test_<module>.cpp`:
   - Add synthetic in-memory buffer tests covering happy paths, out-of-bounds/truncated buffers, invalid magic/flags, and 16-byte alignment edge cases.

### Step 7: Build `open-vfs.exe` & Run CTest
Inside `.open-vfs-repo/`:
```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
python tools/progress.py --scan
```
Confirm:
- `open-vfs.exe` compiles with zero errors.
- All CTest targets pass.
- `<symbol>` now appears as `"status": "impl"` in `--scan`.
- `git -C .open-vfs-repo status` lists only intended files in `src/<module>/` and `tests/test_<module>.cpp`.

### Step 8: Push & Open Pull Request
```powershell
git -C .open-vfs-repo add src/<module>/ tests/test_<module>.cpp
git -C .open-vfs-repo commit -m "feat(<module>): implement <symbol>"
git -C .open-vfs-repo push -u origin feat/m<num>-<module>-<symbol>
gh pr create --repo penwin2000/open-vfs `
  --title "feat(<module>): implement <symbol>" `
  --body "Closes #<issue>`n`nImplements \`<module>::<symbol>\` (clean-room C++20) and adds synthetic-buffer CTest coverage in \`tests/test_<module>.cpp\`."
```
Before finalizing, re-read `git diff` and the PR body to guarantee zero mention of local filenames, host paths, commercial game titles, franchises, studios, or external reference repositories.
