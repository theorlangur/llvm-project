# clangd Unity-Build & Hierarchical PCH Extension (`nov2026`)

## Overview
Built on LLVM `llvmorg-23.1.6`, this branch extends `clangd` for large legacy C++ unity-build codebases using hierarchical `stdafx`-style precompiled headers—without preprocessing `compile_commands.json`:
- **`UnityProjectCDB`**: Automatically discovers unity translation units, hierarchical parent-to-child PCH chains, and both `#pragma once` and `#ifndef`-guarded headers from standard `compile_commands.json` (passing non-unity projects through transparently). Also handles projects using CMake `target_precompile_headers` (`cmake_pch` wrappers), `-include-pch`, or `clang-cl` `/Yu`/`/Yc`/`/Fp` by stripping external `.pch` flags and unwrapping PCH headers cleanly.
- **`PCHManager` v2**: Builds hierarchical static `.pch_cache` files asynchronously and synthesizes small in-memory chained dynamic PCHs for individual non-self-contained headers (truncated just before the target header via `PPSkipIncludes`).
- **Workspace Symbol Search (`--workspace-symbol-fuzzy-sw`)**: Optional generation-cached Smith-Waterman fuzzy matcher across `MemIndex`, `Dex`, and `MergedIndex`.
- **Multi-Invocation Tweaks**: `CreateOutline`, `OverrideVirtualMulti`, `ChangeSignature`, and `AddRemoveMethodConst` via standard `textDocument/codeAction`.

## Build & Quick Start

Configure and build `clangd`:
```bash
./config_for_clangd_nov2026.sh
ninja -C build_nov2026 clangd
```

Run against an unmodified `compile_commands.json` (`--unity-pch` and `--pch` are enabled by default):
```bash
build_nov2026/bin/clangd \
  --compile-commands-dir=/path/to/build \
  --background-index \
  --workspace-symbol-fuzzy-sw
```

## CLI Options

### Unity & PCH Options
| Flag | Default | Description |
| :--- | :--- | :--- |
| `--unity-pch` | `true` | Auto-discover unity TUs and PCH hierarchies from `compile_commands.json`. |
| `--unity-pch-config=<path>` | `""` | Explicit path to JSON override config (auto-loads `unity_pch.json` or `.clangd-unity.json` from project root if omitted). |
| `--pch` | `true` | Enable hierarchical static + dynamic `PCHManager`. |
| `--pch-in-memory` | `true` | Keep built static PCH buffers in memory. |
| `--pch-wait-for-init` | `true` | Wait for initial PCH discovery/build before answering file requests. |
| `--pch-threads=<N>` | `0` | Max concurrent static PCH rebuild workers (`0` = auto). |
| `--pch-max-dynamic=<N>` | `24` | Max in-memory chained dynamic header PCHs retained (LRU-evicted). |
| `--pch-dbg-log` | `false` | Emit verbose `PCHManager` diagnostics. |
| `--cache-dir=<name>` | `"clangd"` | Subdirectory under `<root>/.cache/` for index and `.pch_cache` files. |

### Smith-Waterman Workspace Symbol Options
| Flag | Default | Description |
| :--- | :--- | :--- |
| `--workspace-symbol-fuzzy-sw` | `false` | Use Smith-Waterman fuzzy search for `workspace/symbol`. |
| `--workspace-symbol-fuzzy-sw-threads=<N>` | `0` | Matcher worker threads (`0` = auto). |
| `--workspace-symbol-vscode-name-only` | `false` | Match symbol names only unless `::` appears in the query. |
| `--workspace-symbol-first-space-split-scope-name` | `true` | Split query on first space into `<scope> <name>`. |
| `--workspace-symbol-extended-queries` | `true` | Support leading `!` (upstream search fallback) and `?` (include symbols outside workspace root). |

## Optional `unity_pch.json` / `.clangd-unity.json`

Place in the project source root (or pass `--unity-pch-config`) when filtering paths or granting cross-directory header ownership:

```json
{
  "filter-in": ["src/"],
  "filter-out": ["third_party/"],
  "skip-deps": [".*\\.generated\\.h$"],
  "pch": [
    {
      "file": "src/app/stdafx.hpp",
      "allow-includes-from": ["src/shared/"]
    }
  ]
}
```
