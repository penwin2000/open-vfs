<p align="center">
  <img src="./assets/banner.svg" alt="open-vfs — Clean-Room C++20 and DirectX 12 Standalone Windows 11 Runtime" width="100%" />
</p>

<p align="center">
  <a href="https://github.com/penwin2000/open-vfs/actions/workflows/progress.yml">
    <img src="https://img.shields.io/github/actions/workflow/status/penwin2000/open-vfs/progress.yml?branch=main&style=flat-square&label=Build" alt="Build Status" />
  </a>
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square&logo=cplusplus&logoColor=white" alt="C++20" />
  <img src="https://img.shields.io/badge/Graphics-DirectX%2012-107C10?style=flat-square" alt="DirectX 12" />
  <a href="./LICENSE">
    <img src="https://img.shields.io/badge/License-MIT-2563EB?style=flat-square" alt="MIT License" />
  </a>
</p>

<p align="center">
  <a href="https://penwin2000.github.io/open-vfs/">
    <img src="./progress.svg" alt="open-vfs live subsystem tile map" width="100%" />
  </a>
</p>

---

**`open-vfs`** is a clean-room **C++20** and **DirectX 12** native Windows 11 desktop runtime (`open-vfs.exe`) built from scratch to mount, decode, and execute user-supplied local console asset archives (`.rpf`, `.ttpf`, `.xvd`, `.alignmentchunk`) without relying on any proprietary executable or redistributed data.

> **Clean-Room Policy:** This repository contains **zero** copyrighted assets, binaries, encryption keys, local dump filenames, or commercial identifiers. Contributors supply their own local read-only archive folder at runtime via `--data-dir <path>`.

---

## System Requirements

| Requirement | Minimum / Supported | Notes |
| :--- | :--- | :--- |
| **Operating System** | Windows 11 (x64) | Native Win32 + DXGI 1.6 desktop target |
| **Build System** | CMake 3.25+ | Generates Visual Studio or Ninja build trees |
| **C++ Toolchain** | MSVC (VS 2022) or Ninja + Clang/MSVC | Strict C++20 (`/W4 /permissive-`) |
| **Graphics Hardware** | DirectX 12 Feature Level 12_0+ GPU | Hardware swapchain & pipeline execution |
| **Scripting** | Python 3.10+ | Standard-library-only `tools/progress.py` scanner |
| **Collaboration CLI** | GitHub CLI (`gh`) | Automated Issue -> Branch -> Milestone -> PR flow |

---

## Subsystem Architecture

Source code is strictly partitioned into eight isolated static library modules so concurrent contributors never collide on shared files. See [ARCHITECTURE.md](./ARCHITECTURE.md) for binary layouts and dependency diagrams.

| Module | Directory | Responsibility | Milestone |
| :--- | :--- | :--- | :--- |
| **VFS Core** | `src/vfs_core/` | Virtual mount manager, path resolver, little-endian readers, 16-byte alignment checks | `M1` |
| **RPF9 Parser** | `src/rpf9/` | `0x52504639` (`"RPF9"`) container headers, TOC entries, and compressed blocks | `M1` |
| **RDS5 Parser** | `src/rds5/` | `0x52445335` (`"RDS5"`) headers, resource descriptors, and stream chunks | `M1` |
| **XVD Parser** | `src/xvd/` | `0x1DD3014E` (`"msft-xvd"`) virtual disk volume headers and embedded partitions | `M1` |
| **DX12 Render** | `src/render/` | Native Win32 window loop, DirectX 12 swapchain, texture decoder, and mesh renderer | `M2` |
| **Audio Engine** | `src/audio/` | Windows WASAPI audio mixer and compressed audio stream decoder | `M3` |
| **Input Mapper** | `src/input/` | Gamepad (XInput), Raw Input mouse, and keyboard action mapper | `M3` |
| **World VM** | `src/world_vm/` | Generational entity registry, world loader, physics step, and script bytecode VM | `M3` / `M4` |
| **Desktop App** | `app/main.cpp` | `open-vfs.exe` entry point mounting `--data-dir` and driving the simulation loop | All |

---

## Porting Milestones

| Milestone | Scope | Completion Criteria |
| :--- | :--- | :--- |
| **[M1: VFS & Containers](https://github.com/penwin2000/open-vfs/milestone/1)** | `vfs_core`, `rpf9`, `rds5`, `xvd` | Mount local archive tree, parse TOC/descriptors/partitions, extract aligned streams |
| **[M2: DX12 Render & Assets](https://github.com/penwin2000/open-vfs/milestone/2)** | `render` | Win32 + DX12 swapchain, BC texture decoding, HLSL pipeline, mesh draw submission |
| **[M3: Audio, Input & World VM](https://github.com/penwin2000/open-vfs/milestone/3)** | `audio`, `input`, `world_vm` | WASAPI output + stream codecs, gamepad/KBM input, physics step, bytecode VM execution |
| **[M4: Playable PC Parity](https://github.com/penwin2000/open-vfs/milestone/4)** | Cross-module integration | World sector streaming, native opcode dispatch, 60+ FPS desktop parity |

---

## Quick Start & Build Commands

<details>
<summary><strong>Configure &amp; Build <code>open-vfs.exe</code> (MSVC / Visual Studio 2022)</strong></summary>

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

</details>

<details>
<summary><strong>Configure &amp; Build with Ninja (Developer PowerShell)</strong></summary>

```powershell
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

</details>

<details>
<summary><strong>Run Synthetic-Buffer CTest Suite &amp; Launch <code>open-vfs.exe</code></strong></summary>

```powershell
# Run all 8 isolated module unit test suites
ctest --test-dir build -C Release --output-on-failure

# Launch standalone runtime against your local read-only archive folder
.\build\Release\open-vfs.exe --data-dir "D:\path\to\local_archives"

# Run a single headless mount + container header probe tick
.\build\Release\open-vfs.exe --data-dir "D:\path\to\local_archives" --headless
```

</details>

<details>
<summary><strong>Scan Implementation Progress &amp; Regenerate <code>progress.svg</code></strong></summary>

```powershell
# Emit structured JSON of all OPEN_VFS_IMPL and OPEN_VFS_STUB functions
python tools/progress.py --scan

# Regenerate the dense module tile map SVG
python tools/progress.py --output progress.svg
```

</details>

---

## Contributing & Autonomous Agent Setup

Every task follows the strict **Issue -> Branch -> Milestone -> PR** lifecycle described in [CONTRIBUTING.md](./CONTRIBUTING.md) and [AGENTS.md](./AGENTS.md).

If you open your local archive folder in Antigravity and send:

> `Set up https://github.com/penwin2000/open-vfs and start contributing`

the agent automatically clones this repository into `.open-vfs-repo/`, picks an unclaimed `OPEN_VFS_STUB` symbol, opens a milestone-tracked issue, creates `feat/m<num>-<module>-<symbol>`, inspects matching local archive headers read-only without copying any data, implements the clean-room C++20 function with synthetic CTest coverage, and opens a verified pull request.
