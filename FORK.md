# Cemu fork (Wii U GamePad for Android)

Upstream: https://github.com/cemu-project/Cemu

`main` is upstream `3310f3b8` (2026-09-10) plus one fork commit with the
GamePad work: DRC/capture hooks, Vulkan channel-order and `A2B10G10R10`
unpacking, bounded encode worker, TCP video server on `26761`, and DSU
client probing (`src/input/api/DSU/DSUControllerProvider.*`).

Compare against upstream at any time:

```text
https://github.com/cemu-project/Cemu/compare/main...christianborges121:main
```

or locally with the `upstream` remote:

```powershell
git fetch upstream
git diff upstream/main...main --stat
```

## Dependency pins

Dependencies are real submodules (see `.gitmodules`). Most pins were moved
forward from upstream's for the September 2026 build:

| Dependency | Fork pin | Upstream pin at base |
|:---|:---|:---|
| cubeb | `80dd0a2e` (2026-09-04) | `2071354a` (2023-03-21) |
| imgui | `9aae45eb` (2022-06-21, older than base) | `f65bcf48` (2023-03-23) |
| ZArchive | `965b66c8` (2025-08-30) | `d2c71773` (2022-09-08) |
| Vulkan-Headers | `ee2ec5fd` (2026-09-04) | `01393c3d` (2026-06-12) |
| vcpkg | `a1cae005` (2026-09-11) | `9849e070` (2026-06-02) |
| metal-cpp | `c9727bc9` | `a63bd17f` (2024-01-12) |
| xbyak_aarch64 | `9a271a7c` | `904b8922` (2025-04-05) |

Note: `imgui` is pinned older than upstream's pin. Revisit whether that
downgrade is still needed.

cubeb's nested `cmake/sanitizers-cmake` worktree holds `bcb1fc68` while
cubeb records `aab6948f`; `googletest`, `cubeb-coreaudio-rs`, and
`cubeb-pulse-rs` were never initialized. Sanitizer helpers do not affect
the MSVC Release/RelWithDebInfo build.

## Excluded from version control

Regenerable vcpkg outputs (restore with a normal vcpkg bootstrap/build):

- `dependencies/vcpkg/buildtrees/`
- `dependencies/vcpkg/downloads/`
- `dependencies/vcpkg/packages/`
- `dependencies/vcpkg/vcpkg_installed/`

Build outputs under `bin/` remain covered by the upstream `.gitignore`
(`bin/Cemu_*`, `*.pdb`, `*.ilk`, logs, keys, per-machine state).
