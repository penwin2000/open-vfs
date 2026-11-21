#pragma once

// ============================================================================
// open-vfs Progress Tracking Macros
// ============================================================================
// Every tracked function definition under src/<module>/ MUST be annotated
// with exactly one of these macros immediately before its return type:
//
//   OPEN_VFS_IMPL  -> Clean-room implementation complete with unit test coverage.
//   OPEN_VFS_STUB  -> Pending clean-room implementation (returns Status::NotImplemented
//                     or a documented safe sentinel until completed).
//
// tools/progress.py scans src/ for these tokens to produce JSON status (--scan)
// and regenerate the live module tile map (progress.svg).
// ============================================================================

#define OPEN_VFS_IMPL
#define OPEN_VFS_STUB
