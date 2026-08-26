/*
 * The firmware conductor: step 3 of the coordinator ladder. This node
 * discovers workers on the network, pushes the Mandelbrot module to
 * each from its own flash, and hosts the job — parameters, the shared
 * work counter, and the framebuffer — in its own RAM, serving every
 * remote-memory RPC while the swarm renders. No PC anywhere: progress
 * and the finished fractal go to the serial console.
 *
 * The brain below is plain C written strictly against the peer
 * host-function API (peer_api.h, i32 handles); step 4 lifts the same
 * logic into a WASM module by registering that API as WAMR natives.
 */
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

#include "net/netup.h"
#include "peer_api.h"

LOG_MODULE_REGISTER(wasp_conductor, LOG_LEVEL_INF);

static const uint8_t mandelbrot_wasm[] = {
#include <mandelbrot_module.wasm.inc>
};

/* Job geometry — mirrors tools/mandelbrot_demo.py's defaults. */
#define IMG_W 256
#define IMG_H 192
#define TILE_ROWS 2
#define MAX_ITER 96
#define TILES (IMG_H / TILE_ROWS)
#define Q28 (1 << 28)

#define PARAMS_REGION 1
#define CTR_REGION 2
#define FB_REGION 3

#define NWORKERS CONFIG_WASP_PEER_MAX_PEERS

static uint8_t params[28];
static uint8_t counter[8];
static uint8_t fb[IMG_W * IMG_H];

static int8_t tile_owner[TILES]; /* connection handle, -1 = unrendered */

static int handles[NWORKERS];
static uint8_t pending[NWORKERS]; /* async render call in flight */
static uint8_t dead[NWORKERS];
static int32_t reported[NWORKERS]; /* render's own tile count */
static int nworkers;

static int tiles_done(void)
{
	int done = 0;

	for (int t = 0; t < TILES; t++) {
		done += tile_owner[t] >= 0;
	}
	return done;
}

static void drain_events(void)
{
	wasp_event ev;

	while (wasp_peer_poll_event(&ev, 0) == 1) {
		if (ev.type == WASP_EV_MEM_WRITE && ev.region == FB_REGION) {
			uint32_t tile = ev.offset / (IMG_W * TILE_ROWS);

			if (tile < TILES) {
				tile_owner[tile] = (int8_t)ev.tag;
			}
		}
	}
}

static void log_progress(uint64_t t0)
{
	int counts[NWORKERS] = { 0 };
	char line[NWORKERS * 8 + 1];
	int off = 0;

	for (int t = 0; t < TILES; t++) {
		if (tile_owner[t] >= 0 && tile_owner[t] < NWORKERS) {
			counts[tile_owner[t]]++;
		}
	}
	for (int i = 0; i < nworkers; i++) {
		off += snprintk(line + off, sizeof(line) - off, "%s%d%s",
				i ? "/" : "", counts[handles[i]],
				dead[i] ? "+" : "");
	}
	LOG_INF("%3d/%d tiles  %5u ms  per worker: %s", tiles_done(), TILES,
		(unsigned)(k_uptime_get() - t0), line);
}

/* 64x24 chars: every 4th column of every 8th row, so the character
 * cell aspect (~1:2) restores the image's 4:3. */
static void print_fractal(void)
{
	static const char ramp[] = " .:-=+*#%";
	char line[IMG_W / 4 + 1];

	for (int y = 0; y < IMG_H; y += 8) {
		int x, c;

		for (x = 0, c = 0; x < IMG_W; x += 4, c++) {
			uint8_t v = fb[y * IMG_W + x];

			if (v == 255) {
				line[c] = '?'; /* never rendered */
			} else if (v >= MAX_ITER) {
				line[c] = '@'; /* inside the set */
			} else {
				line[c] = ramp[v * (sizeof(ramp) - 2) /
					       MAX_ITER];
			}
		}
		line[c] = '\0';
		printk("  %s\n", line);
	}
}

static void conduct(void)
{
	int32_t argv[3], res[WASP_CALL_MAX_RESULTS];
	uint64_t t0, last_log;
	int rc, alive;

	memset(tile_owner, -1, sizeof(tile_owner));
	memset(fb, 0xff, sizeof(fb)); /* 255 = unrendered sentinel */
	memset(counter, 0, sizeof(counter));
	sys_put_le32(IMG_W, &params[0]);
	sys_put_le32(IMG_H, &params[4]);
	sys_put_le32(TILE_ROWS, &params[8]);
	sys_put_le32(MAX_ITER, &params[12]);
	/* center (-0.5, 0), real-axis span 3.2, in Q4.28 */
	sys_put_le32((int32_t)(-2.1 * Q28), &params[16]);        /* x0 */
	sys_put_le32((int32_t)(-1.2 * Q28), &params[20]);        /* y0 */
	sys_put_le32((int32_t)((3.2 / IMG_W) * Q28), &params[24]); /* step */

	wasp_export_region(PARAMS_REGION, params, sizeof(params));
	wasp_export_region(CTR_REGION, counter, sizeof(counter));
	wasp_export_region(FB_REGION, fb, sizeof(fb));

	LOG_INF("listening %d s for worker ANNOUNCEs...",
		CONFIG_WASP_CONDUCTOR_DISCOVER_S);
	int nfound = wasp_peer_discover(CONFIG_WASP_CONDUCTOR_DISCOVER_S * 1000);

	if (nfound <= 0) {
		LOG_ERR("CONDUCTOR: FAIL (no workers discovered, rc %d)",
			nfound);
		return;
	}

	for (int i = 0; i < nfound && nworkers < NWORKERS; i++) {
		const wasp_info *info = wasp_peer_info(i);
		int h;

		if (info->busy) {
			LOG_INF("  %s (%s): busy, skipping", info->ip,
				info->board);
			continue;
		}
		h = wasp_peer_connect(i);
		if (h < 0) {
			LOG_WRN("  %s: connect failed (%s)", info->ip,
				wasp_strerror(h));
			continue;
		}
		if (wasp_peer_hello(h, NULL, NULL, NULL) != WASP_OK ||
		    (rc = wasp_peer_load(h, mandelbrot_wasm,
					 sizeof(mandelbrot_wasm))) != WASP_OK) {
			LOG_WRN("  %s: load failed (%s)", info->ip,
				wasp_strerror(rc));
			wasp_peer_close(h);
			continue;
		}
		LOG_INF("  worker %d: %s (%s)", h, info->ip, info->board);
		handles[nworkers++] = h;
	}
	if (nworkers == 0) {
		LOG_ERR("CONDUCTOR: FAIL (no workers usable)");
		return;
	}

	LOG_INF("rendering %dx%d, %d tiles, %d worker(s)", IMG_W, IMG_H, TILES,
		nworkers);
	t0 = last_log = k_uptime_get();

	argv[0] = (int32_t)wasp_pack_ref(PARAMS_REGION, 0);
	argv[1] = (int32_t)wasp_pack_ref(CTR_REGION, 0);
	argv[2] = (int32_t)wasp_pack_ref(FB_REGION, 0);
	for (int i = 0; i < nworkers; i++) {
		rc = wasp_peer_call_async(handles[i], "render", argv, 3);
		if (rc != WASP_OK) {
			LOG_WRN("worker %d: render dispatch failed (%s)",
				handles[i], wasp_strerror(rc));
			dead[i] = 1;
		} else {
			pending[i] = 1;
		}
	}

#ifdef CONFIG_WASP_CONDUCTOR_CHAOS
	int chaos_fired = 0;
#endif

	/* The conduction loop: one thread polls every worker, which
	 * both completes render calls and services the workers'
	 * MEM_READ/MEM_ADD/MEM_WRITE RPCs inline. */
	for (;;) {
		int busy = 0;

		for (int i = 0; i < nworkers; i++) {
			if (!pending[i]) {
				continue;
			}
			rc = wasp_peer_poll(handles[i], res);
			if (rc == WASP_E_AGAIN) {
				busy = 1;
				continue;
			}
			pending[i] = 0;
			if (rc == 1) {
				reported[i] = res[0];
			} else {
				dead[i] = 1;
				LOG_WRN("worker %d died mid-render (%s)",
					handles[i], wasp_strerror(rc));
			}
		}
		drain_events();
		if (!busy) {
			break;
		}

#ifdef CONFIG_WASP_CONDUCTOR_CHAOS
		if (!chaos_fired && nworkers > 1 &&
		    tiles_done() >= TILES / 4) {
			chaos_fired = 1;
			LOG_WRN("chaos: killing worker %d's connection",
				handles[nworkers - 1]);
			wasp_peer_kill(handles[nworkers - 1]);
		}
#endif
		if (k_uptime_get() - last_log >= 1000) {
			last_log = k_uptime_get();
			log_progress(t0);
		}
		k_sleep(K_MSEC(2));
	}
	drain_events();

	/* Re-issue tiles lost with dead workers (claimed off the
	 * counter but never written back). */
	alive = 0;
	for (int i = 0; i < nworkers; i++) {
		alive += !dead[i];
	}
	if (tiles_done() < TILES && alive > 0) {
		int next = 0;

		LOG_INF("re-issuing %d lost tile(s) to %d survivor(s)",
			TILES - tiles_done(), alive);
		for (int t = 0; t < TILES; t++) {
			if (tile_owner[t] >= 0) {
				continue;
			}
			int32_t targv[3] = { argv[0], argv[2], t };

			do {
				next = (next + 1) % nworkers;
			} while (dead[next]);
			rc = wasp_peer_call(handles[next], "render_tile",
					    targv, 3, res);
			if (rc != 1 || res[0] != 1) {
				LOG_WRN("re-issue of tile %d failed (%s)", t,
					wasp_strerror(rc));
			} else {
				tile_owner[t] = (int8_t)handles[next];
			}
		}
		drain_events();
	}

	uint64_t dt = k_uptime_get() - t0;

	for (int i = 0; i < nworkers; i++) {
		if (!dead[i]) {
			wasp_peer_unload(handles[i]);
		}
		wasp_peer_close(handles[i]);
	}

	print_fractal();
	log_progress(t0);
	for (int i = 0; i < nworkers; i++) {
		const wasp_info *info = wasp_peer_conn_info(handles[i]);

		LOG_INF("  worker %d: %-15s %-24s %3d tiles%s", handles[i],
			info ? info->ip : "?", info ? info->board : "",
			(int)reported[i], dead[i] ? "  [died mid-render]" : "");
	}
	if (tiles_done() == TILES) {
		LOG_INF("CONDUCTOR: PASS — %dx%d in %u ms (%u kpix/s), %d workers",
			IMG_W, IMG_H, (unsigned)dt,
			(unsigned)(IMG_W * IMG_H / (dt ? dt : 1)), nworkers);
	} else {
		LOG_ERR("CONDUCTOR: FAIL (%d/%d tiles rendered)", tiles_done(),
			TILES);
	}
}

static void conductor_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

#ifdef CONFIG_WASP_WORKER
	/* The worker's net thread brings the interface up. */
	k_sem_take(&wasp_net_ready, K_FOREVER);
	k_sem_give(&wasp_net_ready);
#else
	wasp_net_up();
#endif
	conduct();
}

K_THREAD_DEFINE(wasp_conductor_tid, CONFIG_WASP_PEER_THREAD_STACK_SIZE,
		conductor_fn, NULL, NULL, NULL, K_PRIO_PREEMPT(8), 0, 0);
