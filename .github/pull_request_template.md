## Summary

Closes #<issue_number>

Brief plain-English description of the clean-room C++20 change in this pull request.

## Issue -> Branch -> Milestone Tracking

| Field | Value |
| :--- | :--- |
| **Linked Issue** | `Closes #<issue_number>` |
| **Milestone** | `M1: VFS & Containers` / `M2: DX12 Render & Assets` / `M3: Audio, Input & World VM` / `M4: Playable PC Parity` |
| **Module Touched** | `src/<module>/` |
| **Branch Name** | `feat/m<num>-<module>-<symbol>` |
| **Symbols Implemented** | `<symbol_name>` (`OPEN_VFS_STUB` -> `OPEN_VFS_IMPL`) |

## Clean-Room & Verification Checklist

- [ ] `cmake --build build --config Release -- /m:1 /nr:false` compiles `open-vfs.exe` and tests with zero errors and zero warnings
- [ ] `ctest --test-dir build -C Release --output-on-failure` passes all synthetic module suites (or verified in CI if local security policy restricts unsigned binaries)
- [ ] Synthetic in-memory unit test coverage added or updated in `tests/test_<module>.cpp`
- [ ] `python tools/progress.py --scan` reflects the updated `OPEN_VFS_IMPL` count
- [ ] Changes stay strictly isolated to the target module and its paired unit test (external forks must not modify `.github/`, `tools/`, `.agent/`, `CMakeLists.txt`, `AGENTS.md`, or `.coderabbit.yaml`)
- [ ] **Clean-Room & Privacy Audit:** Zero binary/archive blobs, zero local machine paths, zero suspicious OS/network calls, and zero commercial game titles, franchises, studios, local dump filenames, or external reference repositories appear anywhere in code, tests, docs, commits, or this PR description
