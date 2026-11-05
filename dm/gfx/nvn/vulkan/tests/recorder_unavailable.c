// The complete NVN public registry includes entry points for the window system, capture and
// other backend features absent from the standalone Vulkan build. A fixture touching one
// must fail. Strong backend implementations supersede these when they become available.
#include "hle.h"
#include "front.h"
#include "nvn_obj.h"
#include "nvn_capture.h"
#include "nvn_mtl.h"

// This standalone fixture has no loader or SDK module and records Switch 1 work.
int dm_sdk_ounce(void) { return 0; }

#define UNAVAILABLE(ret, name, ...) __attribute__((weak)) ret name(__VA_ARGS__) { die("public recorder fixture reached unavailable hook %s", #name); }

UNAVAILABLE(void, dm_cheats_present, void)
UNAVAILABLE(int, dm_detclock_enabled, void)
UNAVAILABLE(void, dm_guest_frame_tick, void)
UNAVAILABLE(int, front_screen_max_fps, void)
UNAVAILABLE(void, mtl_ab_gpu, uint64_t f, double gpu_ms, double frame_ms)
UNAVAILABLE(void, mtl_prog_forget, const prog_t *p)
UNAVAILABLE(int, mtl_probe_ramp_on, void)
UNAVAILABLE(uint64_t, mtl_copy_cut_count, void)
UNAVAILABLE(unsigned, mtl_cpu_tex_count, void)
UNAVAILABLE(void, mtl_cshoist_stats, uint64_t *moved, uint64_t *depth, uint64_t *stops)
UNAVAILABLE(void, mtl_geohoist_stats, uint64_t *moved, uint64_t *runs, uint64_t *stops)
UNAVAILABLE(void, mtl_geomesh_stats, uint64_t *n, uint64_t *wait)
UNAVAILABLE(void, mtl_hybrid_stats, uint64_t *draws, uint64_t *made, uint64_t *none)
UNAVAILABLE(int, mtl_link_rate, void)
UNAVAILABLE(int, mtl_link_stalled, void)
UNAVAILABLE(int, mtl_link_tick, uint64_t n, double *target, double *lead_ms, int *ticks, double *gap_ms)
UNAVAILABLE(int, mtl_link_wait_ticks, int ticks, uint64_t timeout_ns)
UNAVAILABLE(uint64_t, mtl_pkg_edge_skips, void)
UNAVAILABLE(void, mtl_prepare, void)
UNAVAILABLE(void, mtl_prog_prefetch, const prog_t *p)
UNAVAILABLE(void, mtl_pso_line, char *o, size_t n)
UNAVAILABLE(void, mtl_queue_activate, uint64_t serial)
UNAVAILABLE(void, mtl_queue_forget, uint64_t serial)
UNAVAILABLE(void, mtl_set_queue_origin_lower_left, int on)
UNAVAILABLE(void, mtl_sm_stats, uint64_t *np, uint64_t *nc, uint64_t *nw)
UNAVAILABLE(uint64_t, mtl_snap_acquire_wait, void)
UNAVAILABLE(int, mtl_snap_on, void)
UNAVAILABLE(int, mtl_wait_acq, uint64_t fence, uint64_t timeout_ns)
UNAVAILABLE(void, nvn_cap_present, const tex_t *t, const int crop[4])
UNAVAILABLE(void, nvn_cap_submit, int n, const uint64_t *handles)
