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
renamed on both sides together, before the branches are prepared for
upstream review.

## Branches

Each branch starts at a pinned upstream Mesa commit and carries one commit per
patch file from the `amdgpu-wddm` repository (local directory `bc250-win`), in
the order the experiment notes prescribe. Every commit message names its origin
file, that file's SHA-256, whether it matched the committed blob (HEAD 63a97b5
for the 2026-09-26 branches, 1f300d3 for the consolidated ones, 9233583 for
their lifetime commits) and any `git apply` flags. No flags were needed; patch
files stored with CRLF line endings were converted to LF first.

The 2026-09-26 branches are snapshots of the patch files on different
upstream bases. The two `-consolidated` branches of 2026-09-27 carry all of
that work and the later E35 and hosted-runtime patches on one base,
05e6c962: RADV and the ICD side on one branch, the D3D10 UMD on the other.
Each ends with the M546 allocation-lifetime patch, a header-only commit adding
copyright/SPDX lines and, since the `2026-09-27b` heads, small clean-up
commits: the hosted bootstrap ABI version comment, the C4013 comment aligned
between the branches, and the two llvmpipe-era test sources that no
`meson.build` on the UMD branch references removed.

Both `2026-09-27b` heads were built clean on the development PC with the
recorded recipes (zero warnings) and the artifacts passed the E14 compute
smoke and the M546 flip-model regression on the lab, with the recorded control
executable and process router and the baselines restored afterwards
(amdgpu-wddm fact M552, `evidence/windows/2026-09-27-E36-fork-branch-controls`).
ICD `vulkan_radeon.dll` FAD08ECB16C9CFFDCB8D09D408C8ABC428B348D948D945D78DE30892D88A65AF,
UMD `bc250d3d_zink.dll` 848BCBF5FD3B864B183F2194308FA70D254C615B5B30BDBC602C34C9EDC00287.
That qualifies them for those two bounded controls, not for the desktop
compositor probes or for deployment.

| Branch | Base (upstream date) | Patch files (paths in the amdgpu-wddm repository) | Verification against the scratch source tree | Meson configuration |
|---|---|---|---|---|
| `amdgpu-wddm/radv-wddm2-consolidated` | 05e6c962 (2026-09-25) | the nine commits of `amdgpu-wddm/radv-wddm2`, then `evidence/windows/2026-09-26-E35-gfx-submit-pipeline/gather-pipeline.patch`, `experiments/E35-gfx-submit-pipeline/radv-prototype-contract.patch`, `experiments/E34-native-d3d-zink/hosted-runtime/icd.patch` (16 copyright hunks emptied, see below), `runtime-import-icd.patch`, `borrowed-residency.patch`, `present-icd.patch`, `loss-icd.patch`, `lifetime-icd.patch`; header commit; ABI version comment | `m12/mesa-current-src` frozen 2026-09-26T20:22Z: 77 of 86 paths identical; 6 differ only in the copyright sign the tree carries re-encoded, 3 only in the added header. Every intermediate state matches the patch manifests and the saved pre-patch copies; the lifetime commit matches `lifetime-manifest.json` before and after. Clean build FAD08ECB passes E14 smoke and the M546 flip regression (M552) | `radv`; `zink-gl` for the WGL increments |
| `amdgpu-wddm/d3d10umd-consolidated` | 05e6c962 (2026-09-25) | the five `amdgpu-wddm/d3d10umd-llvmpipe` patches and its header commit, the six `amdgpu-wddm/d3d10umd-zink` patches (the first as file states, see below), then `experiments/E35-gfx-submit-pipeline/umd-analysis-fixes.patch`, `experiments/E34-native-d3d-zink/hosted-runtime/umd.patch`, `persistent-screen.patch`, `runtime-import-umd.patch`, `present-umd.patch`, `loss-umd.patch`, `lifetime-umd.patch`; header commit; C4013 comment, ABI version comment, test sources removed | `m13-native-zink-src` frozen 2026-09-26T20:22Z: 30 of 36 paths identical; 3 hold llvmpipe-era E26 changes the Zink tree lacked at the time (`tgsi_to_nir.c`, whose SAMPLE/SAMPLER_VIEW lowering the desktop compositor's shaders need, M548; `tgsi_exec.c`; `gdi_sw_winsys.c`), 1 only the added header. Every intermediate state matches the patch manifests and the saved pre-patch copies; the lifetime commit matches `lifetime-manifest.json`. Clean build 848BCBF5 passes the M546 flip regression (M552) | `zink-umd` at the head; `llvmpipe-umd` up to the header commit of the llvmpipe series |
| `amdgpu-wddm/radv-wddm2` | 05e6c962 (2026-09-25) | `experiments/E33-m12-applications/mesa05-wddm2.patch`, then eight increments: E33 `zink-wgl-zero-client`, `zink-wgl-front-back`, `zink-pbo-teardown`, `radv-scratch-canonical-va`, `zink-wgl-flush-validation`, `wsi-cpu-acquire-rotation`; E34 `radv-lb7a-import`, `hosted-paging-icd` | `m12/mesa-current-src`: 81 changed paths identical, 4 differ (in-flight E35 work, see below) | `radv`; `zink-gl` for the WGL increments |
| `amdgpu-wddm/radv-wddm2-baseline-9c40083c` | f333dd6d (2026-09-24) | `experiments/E27-m9-inference/radv-main/mesa-main-wddm2-bc250.patch`, `experiments/E31-vulkan-wsi/wsi-cpu-fifo.patch` | `mesa-radv-main-20260924`: all 75 changed paths identical; that tree's build output is the deployed ICD 9C40083C | `radv` |
| `amdgpu-wddm/radv-wddm2-lfrb-801c976` (local only) | lfrb/wddm2 801c9763 (2026-07-23) | `driver/icd/mesa-wddm2-bc250.patch`, `driver/icd/mesa-wddm2-cache-intent.patch` | `mesa-wddm2`: all 12 patched RADV files identical; 18 d3d10umd, TGSI and GDI files differ (earlier E26 desktop prototypes, not imported) | `radv` (historical, 26.2.0-devel) |
| `amdgpu-wddm/d3d10umd-llvmpipe` | f9a2d34a (2026-09-24) | `experiments/E26-wddm-desktop/mesa-main-bc250-gallium.patch`, `mesa-shared-cpu-cache-v2.patch`, `mesa-bounded-diagnostics.patch`, `mesa-surface-padding.patch`; `experiments/E32-m11-robustness/mesa-resource-close.patch` | `mesa-main-20260924`: all 18 changed paths identical; that tree's build output is the deployed desktop UMD 8279AC7F | `llvmpipe-umd` |
| `amdgpu-wddm/d3d10umd-zink` | 05e6c962 (2026-09-25) | `experiments/E34-native-d3d-zink/mesa-native-zink-prototype.patch`, `native-shared-import.patch`, `runtime-callback-probe.patch`, `device-owned-and-allocate-abi.patch`, `private-zink-instance.patch`, `hosted-paging-umd.patch` | `m13-native-zink-src`: 19 changed paths identical, 6 differ (in-flight E35 `umd-analysis-fixes.patch`) | `zink-umd` |

The meson configuration names are the ones in `tools/build/mesa-configs.json`
and `docs/build.md` of the amdgpu-wddm repository.

Tags `amdgpu-wddm-<branch name without the prefix>-2026-09-26` mark each
branch head of that day; `amdgpu-wddm-radv-wddm2-consolidated-2026-09-27` and
`amdgpu-wddm-d3d10umd-consolidated-2026-09-27` mark the first assembly of the
consolidated branches (with the in-flight commits);
`amdgpu-wddm-radv-wddm2-consolidated-2026-09-27b` (940ab0eb) and
`amdgpu-wddm-d3d10umd-consolidated-2026-09-27b` (71f2e28c) mark the reworked
heads that were built and controlled. `amdgpu-wddm-radv-wddm2-mesa05-2026-09-26` marks the first commit
of `amdgpu-wddm/radv-wddm2`, which is `mesa05-wddm2.patch` alone: the source of
the M12.1 candidate ICD (lab ICD 4D027149).

Verification compared each branch's files with the scratch tree it was built
from, after normalising CRLF to LF. It covered every file named in the patch
headers, 20 untouched files and a full scan of every path either side changes
relative to the base. The reports are in the workspace under
`scratch/mesa-fork-2026-09-26/verify-*.txt`. The consolidated branches were
checked the same way against a frozen copy of each tree, plus each
intermediate commit against the before/after hashes of the hosted-runtime
patch manifests and the pre-patch file copies saved during that work
(`scratch/mesa-fork-2026-09-27/verify-*-consolidated.txt`, `intermediate-*.txt`).

### How the consolidated branches were assembled

- `icd.patch` records that the M539 edit saved six UTF-8 files through
  cp1250, turning the copyright sign on 16 lines into its re-encoded form.
  The branch applies the patch with the UTF-8 sign on both sides of those
  lines, so no copyright line changes; re-encoding them in the result
  reproduces the manifest hashes. The scratch tree still has the re-encoded
  signs.
- `mesa-native-zink-prototype.patch` was generated against unmodified
  05e6c962 and already contains the E26/E32 d3d10umd frontend changes, so it
  does not apply after the llvmpipe commits. Its commit sets the 17 paths the
  patch touches to their state in `amdgpu-wddm/d3d10umd-zink` (c3066143);
  every other patch applied with `git apply --index` and no flags.
- The E26/E32 patches were written for f9a2d34a; every upstream file they
  touch has the same blob in 05e6c962, and the result equals the published
  llvmpipe commits file for file.

## Known gaps at the snapshot

- The two consolidated heads are built and controlled separately (M552); a
  combined tree has not been built. `meson.build` differs between them in
  disjoint hunks (RADV: `with_vulkan_dx` and `HAVE_VULKAN_DX`; UMD: the
  d3d10umd swrast-or-zink check) with the `/we4013` line now identical, so a
  three-way merge of that file is clean, but its result is untested.
- The lifetime commits at the end of each branch reproduce the recorded
  `lifetime-*.patch` files exactly (the earlier in-flight commits turned out
  to be the same edits). `radv_wddm2_bo.c` hashes to the manifest values only
  after re-encoding its copyright sign the way the scratch tree stores it.
- The E33 WGL/Zink increments (`zink_kopper.c`, `stw_*.c`,
  `st_pbo_compute.c`) and the `dzn` changes are only on the RADV branch; the
  UMD branch leaves those files at upstream.
- The head of `amdgpu-wddm/d3d10umd-consolidated` keeps the E26 changes to
  `tgsi_to_nir.c`, `tgsi_exec.c` and `gdi_sw_winsys.c`. The `tgsi_to_nir.c`
  change (D3D10 SAMPLE opcodes with separate sampler views) is on the Zink
  shader path and is kept on purpose: the desktop compositor's pixel shaders
  stop in `tgsi_to_nir` without it (amdgpu-wddm fact M548), and the lab's
  Zink UMD has since imported the same patch. The two llvmpipe-era test
  sources were removed; the head builds only the Zink UMD, and the llvmpipe
  UMD builds from the llvmpipe part of the series or from
  `amdgpu-wddm/d3d10umd-llvmpipe`.
- The scratch tree `m12/mesa-current-src` carries the cp1250-re-encoded
  copyright sign in six files; the branches use UTF-8.
- The head of `amdgpu-wddm/radv-wddm2` still calls `radv_wddm2_winsys_create`
  from `radv_device.c` with three arguments and without its public header;
  the consolidated branch carries the E35 correction and makes C4013 fatal.
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
