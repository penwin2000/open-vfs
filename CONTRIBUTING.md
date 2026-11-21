# Contributing to `open-vfs`

Thank you for helping build **`open-vfs`**, a clean-room C++20 and DirectX 12 native Windows 11 runtime. This project is structured so multiple humans and coding agents can implement subsystems concurrently without merge conflicts or legal/privacy exposure.

---

## Clean-Room & Privacy Rules (Non-Negotiable)

| Rule | Requirement |
| :--- | :--- |
| **No Proprietary Data** | Never commit, copy, or paste local dump files (`*.rpf`, `*.ttpf`, `*.xvd`, `*.bin`, `*.dll`, `*.exe`, `*.pri`, `*.xml`, `*.config`, `*.png`, `*.alignmentchunk`). |
| **No Identifiers** | Never mention any commercial game title, franchise, studio, local dump filename, or external reference repository in code, comments, SVGs, commits, issues, or PRs. |
| **Synthetic Tests Only** | All CTest unit tests under `tests/` must construct small in-memory synthetic buffers—never load files from a local dump. |
| **Read-Only Inspection** | Local archive headers may be inspected read-only to understand structural layouts and alignment; all implementations must be original clean-room C++20. |

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

## Issue -> Branch -> Milestone -> PR Workflow

1. **Discover an unclaimed stub** using `python tools/progress.py --scan`, `gh issue list`, and `gh pr list`.
2. **Open a GitHub Issue** assigned to the matching Milestone (`M1`–`M4`) using `.github/ISSUE_TEMPLATE/task.md`.
3. **Create a dedicated branch** from `main` named `feat/m<num>-<module>-<symbol>` (lowercase, hyphens for separators).
4. **Implement & Test** your function strictly within `src/<module>/` and `tests/test_<module>.cpp`.
5. **Verify locally** that `open-vfs.exe` compiles cleanly, `ctest --output-on-failure` passes, and `git status` shows zero ignored/dump files.
6. **Open a Pull Request** referencing `Closes #<issue>` and complete the PR checklist.

<details>
<summary><strong>Step-by-Step CLI Walkthrough (GitHub CLI + CMake + CTest)</strong></summary>

```powershell
# 1. Inspect open work and available stubs
python tools/progress.py --scan
gh issue list --repo penwin2000/open-vfs --state open
gh pr list --repo penwin2000/open-vfs --state open

# 2. Claim a stub by opening a milestone-assigned issue
gh issue create --repo penwin2000/open-vfs `
  --title "[rpf9] Implement parse_rpf9_toc_table" `
  --milestone "M1: VFS & Containers" `
  --body "Implement clean-room TOC table parser and synthetic buffer tests for RPF9."

# 3. Branch from latest main
git checkout main
git pull origin main
git checkout -b feat/m1-rpf9-parse-rpf9-toc-table

# 4. Build open-vfs.exe and verify CTest
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure

# 5. Confirm macro status update and clean git tree
python tools/progress.py --scan
git status

# 6. Push and open a PR linked to the issue
git push -u origin feat/m1-rpf9-parse-rpf9-toc-table
gh pr create --repo penwin2000/open-vfs `
  --title "feat(rpf9): implement parse_rpf9_toc_table" `
  --body "Closes #<issue_number>`n`nImplements clean-room RPF9 TOC table parsing with synthetic CTest coverage."
```

</details>
