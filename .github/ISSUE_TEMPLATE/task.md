---
name: Porting Task (OPEN_VFS_STUB)
about: Claim an unclaimed OPEN_VFS_STUB symbol under the Issue -> Branch -> Milestone workflow
title: "[<module>] Implement <symbol>"
labels: []
assignees: []
---

Claim-Key: <module>::<symbol>

## Task Scope

| Field | Value |
| :--- | :--- |
| **Target Module** | `src/<module>/` (`vfs_core` / `rpf9` / `rds5` / `xvd` / `render` / `audio` / `input` / `world_vm`) |
| **Target Symbol** | `<symbol_name>` (currently tagged `OPEN_VFS_STUB`) |
| **Milestone** | `M1: VFS & Containers` / `M2: DX12 Render & Assets` / `M3: Audio, Input & World VM` / `M4: Playable PC Parity` |
| **Branch Name** | `feat/m<num>-<module>-<symbol>` |

## Clean-Room Specification

Describe the binary layout, alignment invariant, or native Windows 11 API behavior required for this function without referencing any commercial titles, studios, or local archive filenames.

- **Inputs / Preconditions:**
- **Outputs / Error Statuses:**
- **Alignment & Bounds Invariants:**

## Acceptance Criteria

- [ ] Symbol flipped from `OPEN_VFS_STUB` to `OPEN_VFS_IMPL` in `src/<module>/`
- [ ] Changes stay strictly isolated to `src/<module>/` and `tests/test_<module>.cpp`
- [ ] Synthetic-buffer unit tests added in `tests/test_<module>.cpp`
- [ ] `open-vfs.exe` compiles cleanly and `ctest --output-on-failure` passes
- [ ] `python tools/progress.py --scan` confirms the symbol is reported as `impl`
- [ ] Zero local dump data, filenames, or commercial references included
