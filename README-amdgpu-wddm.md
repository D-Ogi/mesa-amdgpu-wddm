# amdgpu-wddm: the Mesa side

amdgpu-wddm is an open Windows (WDDM) driver stack for AMD GPUs. Its first
target is the ASRock BC-250 (gfx1013, Cyan Skillfish, VRAM carve-out, no
resizable BAR). This repository holds the Mesa side of that stack: the RADV
Vulkan ICD with its WDDM2 winsys, and the Gallium `d3d10umd` user-mode display
drivers (llvmpipe for the current desktop, Zink as the native GPU prototype).
The kernel-mode driver, the private ABI headers, the experiments and the
evidence live in the separate `amdgpu-wddm` repository.

PROVENANCE: Mesa, MIT. The amdgpu-wddm changes on the `amdgpu-wddm/*` branches
are offered under the same MIT terms as Mesa (see `docs/license.rst`), so that
upstream can take them. Copyright (c) 2026 D-Ogi for those changes. Not
affiliated with or endorsed by AMD, ASRock or Microsoft.

Published at https://github.com/D-Ogi/mesa-amdgpu-wddm, a fork of the GitHub
mirror `intel-lgci-fdo-gitlab-mirror/mesa.mesa` of the upstream repository; the
`amdgpu-wddm/*` branches and their tags are the content, `main` is upstream's.

Identifiers in the code still carry the `bc250` prefix (`bc250_host_bootstrap.h`,
`BC250_TRACE_SUBMITS`, the `bc250d3d` DLL names, the BC2A/BC2C/BC2S private data
blocks). They are a shared contract with the kernel-mode driver and will be
renamed on both sides together, before the branches are consolidated for
upstream review.

## Branches

Each branch starts at a pinned upstream Mesa commit and carries one commit per
patch file from the `amdgpu-wddm` repository (local directory `bc250-win`), in
the order the experiment notes prescribe. Every commit message names its origin
file, that file's SHA-256, whether it matched the committed blob (HEAD 63a97b5)
and any `git apply` flags. No flags were needed; patch files stored with CRLF
line endings were converted to LF first.

The branches are snapshots of the patch files as of 2026-09-26. They sit on
different upstream bases. Consolidating them onto one base is future work.

| Branch | Base (upstream date) | Patch files (paths in the amdgpu-wddm repository) | Verification against the scratch source tree | Meson configuration |
|---|---|---|---|---|
| `amdgpu-wddm/radv-wddm2` | 05e6c962 (2026-09-25) | `experiments/E33-m12-applications/mesa05-wddm2.patch`, then eight increments: E33 `zink-wgl-zero-client`, `zink-wgl-front-back`, `zink-pbo-teardown`, `radv-scratch-canonical-va`, `zink-wgl-flush-validation`, `wsi-cpu-acquire-rotation`; E34 `radv-lb7a-import`, `hosted-paging-icd` | `m12/mesa-current-src`: 81 changed paths identical, 4 differ (in-flight E35 work, see below) | `radv`; `zink-gl` for the WGL increments |
| `amdgpu-wddm/radv-wddm2-baseline-9c40083c` | f333dd6d (2026-09-24) | `experiments/E27-m9-inference/radv-main/mesa-main-wddm2-bc250.patch`, `experiments/E31-vulkan-wsi/wsi-cpu-fifo.patch` | `mesa-radv-main-20260924`: all 75 changed paths identical; that tree's build output is the deployed ICD 9C40083C | `radv` |
| `amdgpu-wddm/radv-wddm2-lfrb-801c976` (local only) | lfrb/wddm2 801c9763 (2026-07-23) | `driver/icd/mesa-wddm2-bc250.patch`, `driver/icd/mesa-wddm2-cache-intent.patch` | `mesa-wddm2`: all 12 patched RADV files identical; 18 d3d10umd, TGSI and GDI files differ (earlier E26 desktop prototypes, not imported) | `radv` (historical, 26.2.0-devel) |
| `amdgpu-wddm/d3d10umd-llvmpipe` | f9a2d34a (2026-09-24) | `experiments/E26-wddm-desktop/mesa-main-bc250-gallium.patch`, `mesa-shared-cpu-cache-v2.patch`, `mesa-bounded-diagnostics.patch`, `mesa-surface-padding.patch`; `experiments/E32-m11-robustness/mesa-resource-close.patch` | `mesa-main-20260924`: all 18 changed paths identical; that tree's build output is the deployed desktop UMD 8279AC7F | `llvmpipe-umd` |
| `amdgpu-wddm/d3d10umd-zink` | 05e6c962 (2026-09-25) | `experiments/E34-native-d3d-zink/mesa-native-zink-prototype.patch`, `native-shared-import.patch`, `runtime-callback-probe.patch`, `device-owned-and-allocate-abi.patch`, `private-zink-instance.patch`, `hosted-paging-umd.patch` | `m13-native-zink-src`: 19 changed paths identical, 6 differ (in-flight E35 `umd-analysis-fixes.patch`) | `zink-umd` |

The meson configuration names are the ones in `tools/build/mesa-configs.json`
and `docs/build.md` of the amdgpu-wddm repository.

Tags `amdgpu-wddm-<branch name without the prefix>-2026-09-26` mark each
branch head. `amdgpu-wddm-radv-wddm2-mesa05-2026-09-26` marks the first commit
of `amdgpu-wddm/radv-wddm2`, which is `mesa05-wddm2.patch` alone: the source of
the M12.1 candidate ICD (lab ICD 4D027149).

Verification compared each branch's files with the scratch tree it was built
from, after normalising CRLF to LF. It covered every file named in the patch
headers, 20 untouched files and a full scan of every path either side changes
relative to the base. The reports are in the workspace under
`scratch/mesa-fork-2026-09-26/verify-*.txt`.

## Known gaps at the snapshot

- The head of `amdgpu-wddm/radv-wddm2` calls `radv_wddm2_winsys_create` from
  `radv_device.c` with three arguments and without its public header, while the
  function takes four. MSVC accepts the implicit declaration. The E35
  `radv-prototype-contract.patch` corrects it and adds `/we4013`; it was still
  being revised on 2026-09-26 and is not on this branch.
- The E35 per-slot gather ring in `radv_wddm2_cs.c` and `radv_wddm2_cs.h` exists
  only in the scratch tree, with no patch file yet.
- `amdgpu-wddm/d3d10umd-zink` contains no RADV changes. The shared header
  `src/util/bc250_host_bootstrap.h` has the same blob on this branch and on
  `amdgpu-wddm/radv-wddm2`.
- `src/amd/compiler/aco_disass.cpp` and `src/amd/vulkan/winsys/common/radv_winsys_cs.h`,
  carried over from the `lfrb/wddm2` port, have no SPDX header; the amdgpu-wddm
  sources do.
- `amdgpu-wddm/radv-wddm2-lfrb-801c976` is based on a commit that exists only in
  the `lfrb` fork on gitlab.freedesktop.org, so it is not published on GitHub
  until that history is fetched in full.

## Remotes and history

- `upstream`: https://gitlab.freedesktop.org/mesa/mesa.git
- `lfrb`: https://gitlab.freedesktop.org/lfrb/mesa.git (`lfrb/wddm2` present locally)
- `gfxstrand`: https://gitlab.freedesktop.org/gfxstrand/mesa.git (no refs fetched)

The local clone is shallow. Each base commit is present at depth 1, taken from
the workspace's reference clone without network access. `git fetch --unshallow
upstream` restores the full upstream history.
