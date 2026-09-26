# BC-250 Mesa fork

This repository is the Mesa side of the BC-250 Windows driver stack: the RADV
Vulkan ICD with its WDDM2 winsys, and the Gallium `d3d10umd` user-mode display
drivers (llvmpipe for the current desktop, Zink as the native GPU prototype).
The kernel-mode driver, the private ABI headers, the experiments and the
evidence live in the separate `bc250-win` repository.

PROVENANCE: Mesa, MIT.

## Branches

Each branch starts at a pinned upstream Mesa commit and carries one commit per
patch file from `bc250-win`, in the order the experiment notes prescribe. Every
commit message names its origin file, that file's SHA-256, whether it matched
the committed `bc250-win` blob (HEAD 63a97b5) and any `git apply` flags. No
flags were needed; patch files stored with CRLF line endings were converted to
LF first.

The branches are snapshots of the `bc250-win` patch files as of 2026-09-26.
They sit on four different upstream bases. Consolidating them onto one base is
future work.

| Branch | Base (upstream date) | Patch files from bc250-win | Verification against the scratch source tree | Meson configuration |
|---|---|---|---|---|
| `bc250/radv-wddm2` | 05e6c962 (2026-09-25) | `experiments/E33-m12-applications/mesa05-wddm2.patch`, then eight increments: E33 `zink-wgl-zero-client`, `zink-wgl-front-back`, `zink-pbo-teardown`, `radv-scratch-canonical-va`, `zink-wgl-flush-validation`, `wsi-cpu-acquire-rotation`; E34 `radv-lb7a-import`, `hosted-paging-icd` | `m12/mesa-current-src`: 81 changed paths identical, 4 differ (in-flight E35 work, see below) | `radv`; `zink-gl` for the WGL increments |
| `bc250/radv-wddm2-baseline-9c40083c` | f333dd6d (2026-09-24) | `experiments/E27-m9-inference/radv-main/mesa-main-wddm2-bc250.patch`, `experiments/E31-vulkan-wsi/wsi-cpu-fifo.patch` | `mesa-radv-main-20260924`: all 75 changed paths identical; that tree's build output is the deployed ICD 9C40083C | `radv` |
| `bc250/radv-wddm2-lfrb-801c976` | lfrb/wddm2 801c9763 (2026-07-23) | `driver/icd/mesa-wddm2-bc250.patch`, `driver/icd/mesa-wddm2-cache-intent.patch` | `mesa-wddm2`: all 12 patched RADV files identical; 18 d3d10umd, TGSI and GDI files differ (earlier E26 desktop prototypes, not imported) | `radv` (historical, 26.2.0-devel) |
| `bc250/d3d10umd-llvmpipe` | f9a2d34a (2026-09-24) | `experiments/E26-wddm-desktop/mesa-main-bc250-gallium.patch`, `mesa-shared-cpu-cache-v2.patch`, `mesa-bounded-diagnostics.patch`, `mesa-surface-padding.patch`; `experiments/E32-m11-robustness/mesa-resource-close.patch` | `mesa-main-20260924`: all 18 changed paths identical; that tree's build output is the deployed desktop UMD 8279AC7F | `llvmpipe-umd` |
| `bc250/d3d10umd-zink` | 05e6c962 (2026-09-25) | `experiments/E34-native-d3d-zink/mesa-native-zink-prototype.patch`, `native-shared-import.patch`, `runtime-callback-probe.patch`, `device-owned-and-allocate-abi.patch`, `private-zink-instance.patch`, `hosted-paging-umd.patch` | `m13-native-zink-src`: 19 changed paths identical, 6 differ (in-flight E35 `umd-analysis-fixes.patch`) | `zink-umd` |

The meson configuration names are the ones in `bc250-win/tools/build/mesa-configs.json`
and `docs/build.md`.

Tags `bc250-<branch name without the bc250/ prefix>-2026-09-26` mark each
branch head. `bc250-radv-wddm2-mesa05-2026-09-26` marks the first commit of
`bc250/radv-wddm2`, which is `mesa05-wddm2.patch` alone: the source of the
M12.1 candidate ICD (lab ICD 4D027149).

Verification compared each branch's files with the scratch tree it was built
from, after normalising CRLF to LF. It covered every file named in the patch
headers, 20 untouched files and a full scan of every path either side changes
relative to the base. The reports are in the workspace under
`scratch/mesa-fork-2026-09-26/verify-*.txt`.

## Known gaps at the snapshot

- The head of `bc250/radv-wddm2` calls `radv_wddm2_winsys_create` from
  `radv_device.c` with three arguments and without its public header, while the
  function takes four. MSVC accepts the implicit declaration. The E35
  `radv-prototype-contract.patch` corrects it and adds `/we4013`; it was still
  being revised on 2026-09-26 and is not on this branch.
- The E35 per-slot gather ring in `radv_wddm2_cs.c` and `radv_wddm2_cs.h` exists
  only in the scratch tree, with no patch file yet.
- `bc250/d3d10umd-zink` contains no RADV changes. The shared header
  `src/util/bc250_host_bootstrap.h` has the same blob on this branch and on
  `bc250/radv-wddm2`.

## Remotes and history

- `upstream`: https://gitlab.freedesktop.org/mesa/mesa.git
- `lfrb`: https://gitlab.freedesktop.org/lfrb/mesa.git (`lfrb/wddm2` present locally)
- `gfxstrand`: https://gitlab.freedesktop.org/gfxstrand/mesa.git (no refs fetched)

The clone is shallow. Each base commit is present at depth 1, taken from the
workspace's reference clone without network access. `git fetch --unshallow upstream`
restores the full upstream history.
