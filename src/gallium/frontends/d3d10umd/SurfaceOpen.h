// SPDX-License-Identifier: MIT
#pragma once
/*
 * The steps of Bc250EnsureSurface, and the one failed step that OpenResource
 * survives (BD-296).
 *
 * Bc250EnsureSurface records the step that failed in Resource::failedStep.
 * OpenResource reads it. Only one failure is not passed on to the runtime:
 * Lock2 of an opened allocation. The KMD gives an allocation no CPU view
 * when it lives in local memory only: a standard GDI texture (LB7A type 1)
 * while GDI hardware acceleration is on. The GPU compositor reads such a
 * texture with the GPU. This UMD is the CPU compositor, and it can read a
 * surface only through a CPU mapping. Two routes put it in dwm.exe while the
 * KMD reports GDI hardware acceleration: the router's kill switch
 * DwmForceCpu, and a hosted open that failed inside dwm.exe. Until BD-296,
 * OpenResource then set E_INVALIDARG, the runtime removed the compositor's
 * device, and DWM made a new device for the same window again and again
 * (the BD-292 loop).
 *
 * The open now succeeds with an ordinary llvmpipe texture of the same size
 * and format, cleared to zero (transparent black), and the retained log says
 * so. The window shows no content, but the compositor keeps its device and
 * every other window. Every other failed step keeps its error: those are a
 * wrong layout, an exhausted resource or a broken paging queue, and an open
 * without content would hide them.
 */

enum Bc250SurfaceStep {
   BC250_SURFACE_STEP_NONE = 0,
   BC250_SURFACE_STEP_RESOURCE,
   BC250_SURFACE_STEP_SIZE,
   BC250_SURFACE_STEP_PAGING_QUEUE,
   BC250_SURFACE_STEP_FORMAT,
   BC250_SURFACE_STEP_LAYOUT,
   BC250_SURFACE_STEP_ALLOCATE,
   BC250_SURFACE_STEP_MAP_GPU_VA,
   BC250_SURFACE_STEP_MAKE_RESIDENT,
   BC250_SURFACE_STEP_PAGING_FENCE,
   BC250_SURFACE_STEP_LOCK2,
   BC250_SURFACE_STEP_IMPORT,
   BC250_SURFACE_STEPS
};

static inline const char *
Bc250SurfaceStepName(unsigned step)
{
   static const char *const names[BC250_SURFACE_STEPS] = {
      "none", "resource", "size", "paging-queue", "format", "layout", "allocate",
      "map-gpu-va", "make-resident", "paging-fence", "lock2", "import",
   };
   return step < BC250_SURFACE_STEPS ? names[step] : "unknown";
}

/* True when OpenResource keeps the resource as a surface with no content:
 * the resource came from OpenResource (opened), Bc250EnsureSurface failed
 * (hr < 0), and the step that failed is Lock2. */
static inline bool
Bc250OpenWithoutContent(bool opened, unsigned failedStep, long hr)
{
   return opened && hr < 0 && failedStep == BC250_SURFACE_STEP_LOCK2;
}

/* The retained log of an open without content, bounded per process: the
 * first 64 opens, then every 1000th. A CPU desktop with GDI hardware
 * acceleration on opens each new window surface once, for the whole life of
 * DWM (kit r1 of BD-296: 596 opens in one session). */
static inline bool
Bc250NoContentLogged(long n)
{
   return n <= 64 || n % 1000 == 0;
}

/* Whether Bc250EnsureSurface writes its own "BC250 Fail" line for a failed
 * step. Not for Lock2 while OpenResource opens the resource: that failure
 * becomes an open without content, and its line ("BC250 NoContent open",
 * with the Lock2 code) is under the budget above. Without this rule each
 * such open wrote one unbounded Fail line (kit r1: 596 Fail lines for 64
 * NoContent lines). Every other failure keeps its line. */
static inline bool
Bc250SurfaceFailLogged(bool opening, unsigned step)
{
   return !(opening && step == BC250_SURFACE_STEP_LOCK2);
}
