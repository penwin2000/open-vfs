# `open-vfs` Agent Operating Manual

This repository builds **`open-vfs.exe`**, a clean-room **C++20** and **DirectX 12** native Windows 11 desktop runtime for user-supplied local console archive directories.

When a contributor opens their local data directory in Antigravity and says:

> `Set up https://github.com/penwin2000/open-vfs and start contributing`

follow the automated **Issue -> Branch -> Milestone -> PR** workflow below (also packaged in `.agent/skills/contribute/SKILL.md`).

---

## Non-Negotiable Privacy & Clean-Room Guardrails

1. **Never copy or commit proprietary files.** The parent workspace holds raw local archives (`*.rpf`, `*.ttpf`, `*.xvd`, `*.bin`, `*.dll`, `*.exe`, `*.pri`, `*.xml`, `*.config`, `*.png`, `*.alignmentchunk`). Keep all repository operations strictly inside `.open-vfs-repo/`.
2. **Read-only header inspection only.** You may inspect small binary header slices (e.g., first 64–4096 bytes) of local archives in the parent directory strictly read-only to confirm field offsets, magic constants, flags, and 16-byte alignment invariants. Never copy raw bytes, payloads, hash tables, or strings into the repository.
3. **Zero forbidden identifiers.** Never mention any commercial game title, franchise, studio, external reference repository, or local dump filename/path in any source file, comment, test, SVG, commit message, GitHub issue, or pull request.
4. **Module isolation.** Touch **only** the target module directory (`src/<module>/`) and its corresponding unit test (`tests/test_<module>.cpp`) so concurrent contributors never collide.
5. **Synthetic-buffer tests only.** Every unit test must construct small in-memory synthetic byte arrays—never read from the user's local archive directory.

---

## Automated Contributor Workflow

Execute these 8 steps in order:

### 1. Bootstrap `.open-vfs-repo/`
- If `.open-vfs-repo/.git` does not exist in the workspace, clone `https://github.com/penwin2000/open-vfs.git` into `.open-vfs-repo/`.
- Otherwise, `git -C .open-vfs-repo checkout main` and `git -C .open-vfs-repo pull --ff-only origin main`.
- Run all subsequent repository commands with `.open-vfs-repo/` as the working directory.

### 2. Discover an Unclaimed `OPEN_VFS_STUB`
Run these three commands in parallel:
```powershell
python tools/progress.py --scan
gh issue list --repo penwin2000/open-vfs --state open --limit 100
gh pr list --repo penwin2000/open-vfs --state open --limit 100
```
- Cross-reference all `status == "stub"` symbols from `--scan` against open issues and open PRs.
- Select **one** unclaimed `OPEN_VFS_STUB` symbol, prioritizing the earliest incomplete milestone (`M1` -> `M2` -> `M3` -> `M4`):

| Milestone | Exact GitHub Milestone Title | Modules |
| :--- | :--- | :--- |
| `M1` | `M1: VFS & Containers` | `vfs_core`, `rpf9`, `rds5`, `xvd` |
| `M2` | `M2: DX12 Render & Assets` | `render` |
| `M3` | `M3: Audio, Input & World VM` | `audio`, `input`, `world_vm` |
| `M4` | `M4: Playable PC Parity` | Integration & full parity tasks |

### 3. Open a Milestone-Assigned GitHub Issue
Create an issue claiming the chosen symbol via `gh issue create`:
```powershell
gh issue create --repo penwin2000/open-vfs `
  --title "[<module>] Implement <symbol>" `
  --milestone "<Exact GitHub Milestone Title>" `
  --body "Clean-room implementation of \`<module>::<symbol>\` with synthetic-buffer CTest coverage."
```
Record the returned issue number `#<issue>`.

### 4. Create a Dedicated Branch
Create a branch off up-to-date `main` matching `feat/m<num>-<module>-<symbol>` (use lowercase letters, digits, and hyphens):
```powershell
git checkout -b feat/m<num>-<module>-<symbol>
```

### 5. Read-Only Local Header Inspection (When Applicable)
For container parsers (`vfs_core`, `rpf9`, `rds5`, `xvd`) or asset decoders (`render`, `audio`, `world_vm`), inspect matching archive headers in the parent local data folder **read-only** to validate magic values (`0x52504639`, `0x52445335`, `0x1DD3014E`), struct field widths, endianness, and 16-byte alignment constraints. Never copy bytes or record local filenames.

### 6. Implement Clean-Room C++20 & Synthetic Unit Tests
- Implement `<symbol>` in `src/<module>/` using clean, bounds-checked C++20 (`std::span`, `open_vfs::Status`, zero raw pointer overflows).
- Flip its macro tag from `OPEN_VFS_STUB` to `OPEN_VFS_IMPL`.
- Add thorough synthetic-buffer unit tests (valid inputs, malformed/corrupt inputs, alignment/bounds errors) in `tests/test_<module>.cpp`.

### 7. Build & Verify Locally
```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
python tools/progress.py --scan
```
Confirm:
- `open-vfs.exe` compiles cleanly with zero errors.
- All CTest suites pass.
- `python tools/progress.py --scan` shows `<symbol>` as `"status": "impl"`.
- `git status` shows **only** intentional source/test edits under `src/<module>/` and `tests/test_<module>.cpp` (zero ignored or local dump artifacts).

### 8. Commit, Push & Open Linked Pull Request
```powershell
git add src/<module>/ tests/test_<module>.cpp
git commit -m "feat(<module>): implement <symbol>"
git push -u origin feat/m<num>-<module>-<symbol>
gh pr create --repo penwin2000/open-vfs `
  --title "feat(<module>): implement <symbol>" `
  --body "Closes #<issue>`n`nClean-room C++20 implementation of \`<module>::<symbol>\` with synthetic-buffer CTest coverage."
```
Audit the diff, commit message, issue, and PR body one final time to guarantee **zero** mention of local filenames, directory names, commercial titles, franchises, or studios.
