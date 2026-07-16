/*
 * Distributed Mandelbrot renderer — the wasp showcase module.
 * Driven by tools/mandelbrot_demo.py; see docs/demo.md.
 *
 * All math is Q4.28 fixed point (int32 covers ±8.0, the escape radius
 * needs ±4): the Pico W has no FPU, and integer math keeps the
 * interpreter fast and the v1 i32-only calling convention happy.
 *
 * Work distribution is a shared MEM_ADD counter: each node claims the
 * next tile index with one atomic RPC, renders it locally, and ships
 * the finished tile with one bulk MEM_WRITE. No scheduler anywhere —
 * faster nodes simply claim more tiles (see docs/performance.md, T6).
 */
#include "wasp/remote.h"

#define EXPORT(name) __attribute__((export_name(name)))

/*
 * Job parameters, read from the params region as seven i32s. Layout is
 * shared with the coordinator (mandelbrot_demo.py packs it) — fixed
 * width fields only, per the docs/performance.md struct contract.
 */
struct job {
	int width;     /* pixels per row */
	int height;    /* rows */
	int tile_rows; /* rows per work unit */
	int max_iter;  /* <= 254; 255 is the "not rendered yet" sentinel */
	int x0;        /* Q4.28 real part of pixel column 0 */
	int y0;        /* Q4.28 imaginary part of row 0 */
	int step;      /* Q4.28 per-pixel step */
};

static unsigned char buf[8192];

/* Iteration count for c = (cx, cy), Q4.28 in, escape radius 2. */
static int mandel(int cx, int cy, int max_iter)
{
	int x = 0, y = 0;

	for (int i = 0; i < max_iter; i++) {
		long long xx = ((long long)x * x) >> 28;
		long long yy = ((long long)y * y) >> 28;

		if (xx + yy > (4LL << 28)) {
			return i;
		}
		long long xy = ((long long)x * y) >> 27; /* 2xy */

		x = (int)(xx - yy) + cx;
		y = (int)xy + cy;
	}
	return max_iter;
}

/* Render one tile into buf and ship it: exactly one MEM_WRITE. */
static int do_tile(const struct job *j, unsigned fb_ref, int tile)
{
	int row0 = tile * j->tile_rows;
	int rows = j->height - row0;
	unsigned char *p = buf;

	if (rows > j->tile_rows) {
		rows = j->tile_rows;
	}
	if (rows < 1 || j->width * rows > (int)sizeof(buf)) {
		return WASP_REMOTE_EBOUNDS;
	}
	for (int r = 0; r < rows; r++) {
		int cy = j->y0 + (row0 + r) * j->step;
		int cx = j->x0;

		for (int c = 0; c < j->width; c++, cx += j->step) {
			*p++ = (unsigned char)mandel(cx, cy, j->max_iter);
		}
	}
	return wasp_mem_write(fb_ref + (unsigned)(row0 * j->width), buf,
			      (unsigned)(rows * j->width));
}

static int read_job(unsigned params_ref, struct job *j)
{
	int rc = wasp_mem_read(params_ref, j, sizeof(*j));

	if (rc != WASP_REMOTE_OK) {
		return rc;
	}
	if (j->width < 1 || j->height < 1 || j->tile_rows < 1 ||
	    j->max_iter < 1 || j->max_iter > 254) {
		return WASP_REMOTE_EBOUNDS;
	}
	return WASP_REMOTE_OK;
}

/*
 * Main entry: claim tiles off the shared counter until the image is
 * done. Returns the number of tiles this node rendered, or a negative
 * WASP_REMOTE_* error.
 */
EXPORT("render") int render(unsigned params_ref, unsigned ctr_ref, unsigned fb_ref)
{
	struct job j;
	int done = 0;
	int rc = read_job(params_ref, &j);

	if (rc != WASP_REMOTE_OK) {
		return rc;
	}
	int tiles = (j.height + j.tile_rows - 1) / j.tile_rows;

	for (;;) {
		int tile;

		rc = wasp_add(ctr_ref, 1, &tile);
		if (rc != WASP_REMOTE_OK) {
			return rc;
		}
		if (tile >= tiles) {
			return done;
		}
		rc = do_tile(&j, fb_ref, tile);
		if (rc != WASP_REMOTE_OK) {
			return rc;
		}
		done++;
	}
}

/* Render one specific tile — the coordinator's re-issue path for work
 * lost when a node dies mid-render. Returns 1, or a negative error. */
EXPORT("render_tile") int render_tile(unsigned params_ref, unsigned fb_ref, int tile)
{
	struct job j;
	int rc = read_job(params_ref, &j);

	if (rc != WASP_REMOTE_OK) {
		return rc;
	}
	if (tile < 0 || tile * j.tile_rows >= j.height) {
		return WASP_REMOTE_EBOUNDS;
	}
	rc = do_tile(&j, fb_ref, tile);
	return rc == WASP_REMOTE_OK ? 1 : rc;
}
