// SPDX-License-Identifier: GPL-2.0
/*
 * gc5024-camera - expose the Huawei MateBook E GC5024 sensor as a normal
 *                 YUYV webcam through v4l2loopback.
 *
 * The sensor only produces raw Bayer; this does the ISP in software:
 *
 *   /dev/video0 (ip3b, 2592x1944)  ->  unpack 10-bit  ->  2x2 bin
 *     ->  auto white balance + auto exposure  ->  bilinear resize
 *     ->  gamma  ->  YUYV  ->  /dev/video20 (v4l2loopback)
 *
 * Any application that can open a V4L2 capture device (browsers via
 * PipeWire, Cheese, Zoom, ffmpeg, ...) can then use the camera.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/videodev2.h>

/* Sensor geometry: fixed by the GC5024 init table. */
#define GROUP_BYTES	32	/* 25 pixels per group */
#define GROUP_PIX	25

/* Set from --src-width/--src-height at startup. */
static int g_src_w = 2592;
static int g_src_h = 1944;
static int g_src_stride;	/* CIO2: 64 bytes per 50 pixels */
static int g_raw_stride;
static int g_hr_w, g_hr_h;

#define NBUFS		4

struct options {
	const char *src;
	const char *dst;
	const char *media;
	const char *sensor;
	const char *csi2;
	const char *cio2;
	int src_w;
	int src_h;
	int out_w;
	int out_h;
	long frames;
	int no_setup;
	int verbose;
};

struct buffer {
	void *start;
	size_t length;
};

static volatile sig_atomic_t running = 1;

static void on_signal(int sig)
{
	(void)sig;
	running = 0;
}

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do {
		r = ioctl(fd, req, arg);
	} while (r == -1 && errno == EINTR);

	return r;
}

static int run_cmd(char *const argv[])
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		execvp(argv[0], argv);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) < 0)
		return -1;

	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int setup_media(const struct options *o)
{
	char fmt[96], s0[256], c0[256], c1[256], l0[320], l1[320];

	snprintf(fmt, sizeof(fmt), "fmt:SBGGR10_1X10/%dx%d", g_src_w, g_src_h);
	snprintf(s0, sizeof(s0), "\"%s\":0[%s]", o->sensor, fmt);
	snprintf(c0, sizeof(c0), "\"%s\":0[%s]", o->csi2, fmt);
	snprintf(c1, sizeof(c1), "\"%s\":1[%s]", o->csi2, fmt);
	snprintf(l0, sizeof(l0), "\"%s\":0 -> \"%s\":0[1]", o->sensor, o->csi2);
	snprintf(l1, sizeof(l1), "\"%s\":1 -> \"%s\":0[1]", o->csi2, o->cio2);

	char *set0[] = { "media-ctl", "-d", (char *)o->media, "--set-v4l2", s0, NULL };
	char *set1[] = { "media-ctl", "-d", (char *)o->media, "--set-v4l2", c0, NULL };
	char *set2[] = { "media-ctl", "-d", (char *)o->media, "--set-v4l2", c1, NULL };
	char *link0[] = { "media-ctl", "-d", (char *)o->media, "-l", l0, NULL };
	char *link1[] = { "media-ctl", "-d", (char *)o->media, "-l", l1, NULL };
	int rc = 0;

	rc |= run_cmd(set0);
	rc |= run_cmd(set1);
	rc |= run_cmd(set2);
	rc |= run_cmd(link0);
	rc |= run_cmd(link1);

	if (o->verbose)
		printf("media pipeline configured (rc=%d)\n", rc);
	return rc;
}

static int src_open(const struct options *o, struct buffer bufs[NBUFS])
{
	struct v4l2_format fmt = { 0 };
	struct v4l2_requestbuffers req = { 0 };
	struct v4l2_buffer buf = { 0 };
	struct v4l2_plane plane = { 0 };
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	int fd, i;

	fd = open(o->src, O_RDWR);
	if (fd < 0) {
		perror(o->src);
		return -1;
	}

	fmt.type = type;
	fmt.fmt.pix_mp.width = g_src_w;
	fmt.fmt.pix_mp.height = g_src_h;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_IPU3_SBGGR10;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	fmt.fmt.pix_mp.num_planes = 1;
	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
		perror("VIDIOC_S_FMT (source)");
		close(fd);
		return -1;
	}
	if (fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_IPU3_SBGGR10 ||
	    (int)fmt.fmt.pix_mp.width != g_src_w ||
	    (int)fmt.fmt.pix_mp.height != g_src_h) {
		fprintf(stderr, "source refused %dx%d ip3b (got %ux%u %08x)\n",
			g_src_w, g_src_h, fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
			fmt.fmt.pix_mp.pixelformat);
		close(fd);
		return -1;
	}

	req.count = NBUFS;
	req.type = type;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
		perror("VIDIOC_REQBUFS (source)");
		close(fd);
		return -1;
	}

	for (i = 0; i < (int)req.count; i++) {
		memset(&buf, 0, sizeof(buf));
		memset(&plane, 0, sizeof(plane));
		buf.type = type;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		buf.length = 1;
		buf.m.planes = &plane;
		if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
			perror("VIDIOC_QUERYBUF");
			close(fd);
			return -1;
		}
		bufs[i].length = plane.length;
		bufs[i].start = mmap(NULL, plane.length, PROT_READ | PROT_WRITE,
				     MAP_SHARED, fd, plane.m.mem_offset);
		if (bufs[i].start == MAP_FAILED) {
			perror("mmap");
			close(fd);
			return -1;
		}
	}

	for (i = 0; i < (int)req.count; i++) {
		memset(&buf, 0, sizeof(buf));
		memset(&plane, 0, sizeof(plane));
		buf.type = type;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		buf.length = 1;
		buf.m.planes = &plane;
		if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
			perror("VIDIOC_QBUF");
			close(fd);
			return -1;
		}
	}

	if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
		perror("VIDIOC_STREAMON (source)");
		close(fd);
		return -1;
	}

	return fd;
}

static int dst_open(const struct options *o)
{
	struct v4l2_format fmt = { 0 };
	int fd;

	fd = open(o->dst, O_RDWR);
	if (fd < 0) {
		perror(o->dst);
		return -1;
	}

	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.width = o->out_w;
	fmt.fmt.pix.height = o->out_h;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	fmt.fmt.pix.bytesperline = o->out_w * 2;
	fmt.fmt.pix.sizeimage = o->out_w * o->out_h * 2;
	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
		perror("VIDIOC_S_FMT (sink)");
		close(fd);
		return -1;
	}

	if (o->verbose)
		printf("sink: %ux%u %08x, %u bytes/frame\n",
		       fmt.fmt.pix.width, fmt.fmt.pix.height,
		       fmt.fmt.pix.pixelformat, fmt.fmt.pix.sizeimage);

	return fd;
}

/* ---- ISP ---- */

struct isp {
	uint16_t *raw;		/* g_raw_stride * g_src_h */
	uint16_t *hr;		/* g_hr_w * g_hr_h * 3, R then G then B planes */
	uint8_t *yuyv;
	uint8_t lut[3][1024];
	int *mx0, *mx1;
	int *mw;
	int *my0, *my1;
	int *mwy;
	double kr, kg, kb;
	double scale;
};

static inline int clamp255(int v)
{
	return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static void build_maps(struct isp *isp, int out_w, int out_h)
{
	double out_ar = (double)out_w / out_h;
	double src_ar = (double)g_hr_w / g_hr_h;
	double crop_w = g_hr_w, crop_h = g_hr_h;
	double xoff = 0, yoff = 0;
	int i;

	/* Centre-crop to the output aspect ratio instead of stretching. */
	if (out_ar > src_ar) {
		crop_h = g_hr_w / out_ar;
		yoff = (g_hr_h - crop_h) / 2.0;
	} else if (out_ar < src_ar) {
		crop_w = g_hr_h * out_ar;
		xoff = (g_hr_w - crop_w) / 2.0;
	}

	for (i = 0; i < out_w; i++) {
		double f = xoff + ((i + 0.5) * crop_w / out_w) - 0.5;
		int x0, x1, w;

		if (f < 0) f = 0;
		if (f > g_hr_w - 1) f = g_hr_w - 1;
		x0 = (int)f;
		x1 = x0 + 1 < g_hr_w ? x0 + 1 : x0;
		w = (int)((f - x0) * 65536.0);
		isp->mx0[i] = x0;
		isp->mx1[i] = x1;
		isp->mw[i] = w;
	}
	for (i = 0; i < out_h; i++) {
		double f = yoff + ((i + 0.5) * crop_h / out_h) - 0.5;
		int y0, y1, w;

		if (f < 0) f = 0;
		if (f > g_hr_h - 1) f = g_hr_h - 1;
		y0 = (int)f;
		y1 = y0 + 1 < g_hr_h ? y0 + 1 : y0;
		w = (int)((f - y0) * 65536.0);
		isp->my0[i] = y0;
		isp->my1[i] = y1;
		isp->mwy[i] = w;
	}
}

static inline int bilerp(const uint16_t *p, int x0, int x1, int w,
			 int row0, int row1, int wy)
{
	int a = p[row0 + x0], b = p[row0 + x1];
	int c = p[row1 + x0], d = p[row1 + x1];
	int top = a + (((b - a) * w) >> 16);
	int bot = c + (((d - c) * w) >> 16);

	return top + (((bot - top) * wy) >> 16);
}

static void unpack(const uint8_t *src, uint16_t *raw)
{
	int x, y, g, i;

	for (y = 0; y < g_src_h; y++) {
		const uint8_t *line = src + (size_t)y * g_src_stride;
		uint16_t *out = raw + (size_t)y * g_raw_stride;

		for (g = 0; g < g_src_stride / GROUP_BYTES; g++) {
			const uint8_t *gp = line + g * GROUP_BYTES;

			for (i = 0; i < GROUP_PIX; i++) {
				int bit = i * 10;
				int byte = bit >> 3;
				int sh = bit & 7;
				unsigned v = gp[byte] | (gp[byte + 1] << 8);

				out[g * GROUP_PIX + i] = (v >> sh) & 0x3FF;
			}
		}
	}
	(void)x;
}

/* BGGR: (0,0)=B (0,1)=G (1,0)=G (1,1)=R */
static void bin_and_stats(struct isp *isp, int out_w, int out_h,
			  double *mean_r, double *mean_g, double *mean_b)
{
	uint16_t *R = isp->hr;
	uint16_t *G = isp->hr + (size_t)g_hr_w * g_hr_h;
	uint16_t *B = isp->hr + (size_t)g_hr_w * g_hr_h * 2;
	uint64_t sr = 0, sg = 0, sb = 0, n = 0;
	int x, y;

	for (y = 0; y < g_hr_h; y++) {
		const uint16_t *r0 = isp->raw + (size_t)(2 * y) * g_raw_stride;
		const uint16_t *r1 = isp->raw + (size_t)(2 * y + 1) * g_raw_stride;

		for (x = 0; x < g_hr_w; x++) {
			int b = r0[2 * x];
			int g1 = r0[2 * x + 1];
			int g2 = r1[2 * x];
			int r = r1[2 * x + 1];
			int g = (g1 + g2) >> 1;
			size_t idx = (size_t)y * g_hr_w + x;

			R[idx] = r;
			G[idx] = g;
			B[idx] = b;

			if ((x & 7) == 0 && (y & 7) == 0) {
				sr += r;
				sg += g;
				sb += b;
				n++;
			}
		}
	}

	*mean_r = n ? (double)sr / n : 1;
	*mean_g = n ? (double)sg / n : 1;
	*mean_b = n ? (double)sb / n : 1;
	(void)out_w;
	(void)out_h;
}

static void update_gains(struct isp *isp, double mr, double mg, double mb,
			 int frame)
{
	double tr = mg / (mr > 1 ? mr : 1);
	double tb = mg / (mb > 1 ? mb : 1);
	double target = 380.0;		/* mid-grey in 10-bit */
	double want = target / (mg > 1 ? mg : 1);
	int i;

	if (tr < 0.4) tr = 0.4;
	if (tr > 4.0) tr = 4.0;
	if (tb < 0.4) tb = 0.4;
	if (tb > 4.0) tb = 4.0;
	if (want < 0.20) want = 0.20;
	if (want > 8.0) want = 8.0;

	if (!frame) {
		isp->kr = tr;
		isp->kb = tb;
		isp->scale = want;
	} else {
		isp->kr = isp->kr * 0.90 + tr * 0.10;
		isp->kb = isp->kb * 0.90 + tb * 0.10;
		isp->scale = isp->scale * 0.85 + want * 0.15;
	}

	for (i = 0; i < 1024; i++) {
		double v;

		v = i * isp->kr * isp->scale / 1023.0;
		if (v > 1.0) v = 1.0;
		isp->lut[0][i] = (uint8_t)(255.0 * pow(v, 1.0 / 2.2));

		v = i * isp->scale / 1023.0;
		if (v > 1.0) v = 1.0;
		isp->lut[1][i] = (uint8_t)(255.0 * pow(v, 1.0 / 2.2));

		v = i * isp->kb * isp->scale / 1023.0;
		if (v > 1.0) v = 1.0;
		isp->lut[2][i] = (uint8_t)(255.0 * pow(v, 1.0 / 2.2));
	}
}

static void render(struct isp *isp, int out_w, int out_h)
{
	const uint16_t *R = isp->hr;
	const uint16_t *G = isp->hr + (size_t)g_hr_w * g_hr_h;
	const uint16_t *B = isp->hr + (size_t)g_hr_w * g_hr_h * 2;
	int x, y;

	for (y = 0; y < out_h; y++) {
		int y0 = isp->my0[y] * g_hr_w;
		int y1 = isp->my1[y] * g_hr_w;
		int wy = isp->mwy[y];
		uint8_t *line = isp->yuyv + (size_t)y * out_w * 2;

		for (x = 0; x < out_w; x += 2) {
			int x0 = isp->mx0[x], x1 = isp->mx1[x], wx = isp->mw[x];
			int r0 = isp->lut[0][bilerp(R, x0, x1, wx, y0, y1, wy)];
			int g0 = isp->lut[1][bilerp(G, x0, x1, wx, y0, y1, wy)];
			int b0 = isp->lut[2][bilerp(B, x0, x1, wx, y0, y1, wy)];
			int r1, g1, b1, r2, g2, b2, yv0, yv1, u, v;

			if (x + 1 < out_w) {
				x0 = isp->mx0[x + 1];
				x1 = isp->mx1[x + 1];
				wx = isp->mw[x + 1];
				r1 = isp->lut[0][bilerp(R, x0, x1, wx, y0, y1, wy)];
				g1 = isp->lut[1][bilerp(G, x0, x1, wx, y0, y1, wy)];
				b1 = isp->lut[2][bilerp(B, x0, x1, wx, y0, y1, wy)];
			} else {
				r1 = r0; g1 = g0; b1 = b0;
			}

			r2 = (r0 + r1) >> 1;
			g2 = (g0 + g1) >> 1;
			b2 = (b0 + b1) >> 1;

			yv0 = (77 * r0 + 150 * g0 + 29 * b0) >> 8;
			yv1 = (77 * r1 + 150 * g1 + 29 * b1) >> 8;
			u = ((-43 * r2 - 85 * g2 + 128 * b2) >> 8) + 128;
			v = ((128 * r2 - 107 * g2 - 21 * b2) >> 8) + 128;

			line[2 * x + 0] = clamp255(yv0);
			line[2 * x + 1] = clamp255(u);
			line[2 * x + 2] = clamp255(yv1);
			line[2 * x + 3] = clamp255(v);
		}
	}
}

int main(int argc, char **argv)
{
	struct options o = {
		.src = "/dev/video0",
		.dst = "/dev/video20",
		.media = "/dev/media0",
		.sensor = "gc5024 2-0037",
		.csi2 = "ipu3-csi2 0",
		.cio2 = "ipu3-cio2 0",
		.src_w = 2592,
		.src_h = 1944,
		.out_w = 1280,
		.out_h = 960,
		.frames = -1,
	};
	struct buffer bufs[NBUFS] = { 0 };
	struct isp isp = { 0 };
	struct sigaction sa = { 0 };
	struct timespec t0, t1;
	long frames = 0;
	int i, argc_i;

	for (argc_i = 1; argc_i < argc; argc_i++) {
		char *a = argv[argc_i];

		if (!strcmp(a, "--src") && argc_i + 1 < argc)
			o.src = argv[++argc_i];
		else if (!strcmp(a, "--dst") && argc_i + 1 < argc)
			o.dst = argv[++argc_i];
		else if (!strcmp(a, "--media") && argc_i + 1 < argc)
			o.media = argv[++argc_i];
		else if (!strcmp(a, "--sensor") && argc_i + 1 < argc)
			o.sensor = argv[++argc_i];
		else if (!strcmp(a, "--src-width") && argc_i + 1 < argc)
			o.src_w = atoi(argv[++argc_i]);
		else if (!strcmp(a, "--src-height") && argc_i + 1 < argc)
			o.src_h = atoi(argv[++argc_i]);
		else if (!strcmp(a, "--width") && argc_i + 1 < argc)
			o.out_w = atoi(argv[++argc_i]);
		else if (!strcmp(a, "--height") && argc_i + 1 < argc)
			o.out_h = atoi(argv[++argc_i]);
		else if (!strcmp(a, "--frames") && argc_i + 1 < argc)
			o.frames = atol(argv[++argc_i]);
		else if (!strcmp(a, "--no-setup"))
			o.no_setup = 1;
		else if (!strcmp(a, "-v") || !strcmp(a, "--verbose"))
			o.verbose = 1;
		else {
			fprintf(stderr,
				"usage: %s [--src dev] [--dst dev] [--media dev]\n"
				"          [--sensor name] [--src-width N] [--src-height N]\n"
				"          [--width N] [--height N]\n"
				"          [--frames N] [--no-setup] [-v]\n", argv[0]);
			return 2;
		}
	}

	if (o.out_w <= 0 || o.out_h <= 0 || (o.out_w & 1)) {
		fprintf(stderr, "output size must be positive and even in width\n");
		return 2;
	}

	sigemptyset(&sa.sa_mask);
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	g_src_w = o.src_w;
	g_src_h = o.src_h;
	g_src_stride = ((g_src_w + 49) / 50) * 64;
	g_raw_stride = (g_src_stride / GROUP_BYTES) * GROUP_PIX;
	g_hr_w = g_src_w / 2;
	g_hr_h = g_src_h / 2;

	if (!o.no_setup && setup_media(&o) != 0) {
		fprintf(stderr, "warning: media-ctl failed, using current pipeline\n");
	}

	isp.raw = malloc((size_t)g_raw_stride * g_src_h * sizeof(uint16_t));
	isp.hr = malloc((size_t)g_hr_w * g_hr_h * 3 * sizeof(uint16_t));
	isp.yuyv = malloc((size_t)o.out_w * o.out_h * 2);
	isp.mx0 = malloc(o.out_w * sizeof(int));
	isp.mx1 = malloc(o.out_w * sizeof(int));
	isp.mw = malloc(o.out_w * sizeof(int));
	isp.my0 = malloc(o.out_h * sizeof(int));
	isp.my1 = malloc(o.out_h * sizeof(int));
	isp.mwy = malloc(o.out_h * sizeof(int));
	if (!isp.raw || !isp.hr || !isp.yuyv || !isp.mx0 || !isp.mx1 ||
	    !isp.mw || !isp.my0 || !isp.my1 || !isp.mwy) {
		fprintf(stderr, "out of memory\n");
		return 1;
	}
	build_maps(&isp, o.out_w, o.out_h);

	int src = src_open(&o, bufs);
	if (src < 0)
		return 1;

	int dst = dst_open(&o);
	if (dst < 0) {
		close(src);
		return 1;
	}

	clock_gettime(CLOCK_MONOTONIC, &t0);

	while (running && (o.frames < 0 || frames < o.frames)) {
		struct v4l2_buffer buf = { 0 };
		struct v4l2_plane plane = { 0 };
		double mr, mg, mb;
		ssize_t wr;

		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.length = 1;
		buf.m.planes = &plane;
		if (xioctl(src, VIDIOC_DQBUF, &buf) < 0) {
			if (errno == EINTR)
				continue;
			perror("VIDIOC_DQBUF");
			break;
		}

		unpack(bufs[buf.index].start, isp.raw);
		bin_and_stats(&isp, o.out_w, o.out_h, &mr, &mg, &mb);
		update_gains(&isp, mr, mg, mb, frames ? 1 : 0);
		render(&isp, o.out_w, o.out_h);

		wr = write(dst, isp.yuyv, (size_t)o.out_w * o.out_h * 2);
		if (wr < 0 && errno != EAGAIN && errno != EINTR)
			perror("write (loopback)");

		memset(&plane, 0, sizeof(plane));
		buf.length = 1;
		buf.m.planes = &plane;
		if (xioctl(src, VIDIOC_QBUF, &buf) < 0)
			perror("VIDIOC_QBUF");

		frames++;
		if (o.verbose && frames % 30 == 0) {
			clock_gettime(CLOCK_MONOTONIC, &t1);
			double dt = (t1.tv_sec - t0.tv_sec) +
				    (t1.tv_nsec - t0.tv_nsec) / 1e9;
			printf("%ld frames, %.2f fps, gains R=%.2f B=%.2f, "
			       "exposure x%.2f\n", frames, frames / dt,
			       isp.kr, isp.kb, isp.scale);
		}
	}

	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

	xioctl(src, VIDIOC_STREAMOFF, &type);
	for (i = 0; i < NBUFS; i++)
		if (bufs[i].start && bufs[i].start != MAP_FAILED)
			munmap(bufs[i].start, bufs[i].length);
	close(src);
	close(dst);

	clock_gettime(CLOCK_MONOTONIC, &t1);
	{
		double dt = (t1.tv_sec - t0.tv_sec) +
			    (t1.tv_nsec - t0.tv_nsec) / 1e9;
		printf("stopped after %ld frames (%.2f fps average)\n",
		       frames, dt > 0 ? frames / dt : 0);
	}

	free(isp.raw);
	free(isp.hr);
	free(isp.yuyv);
	free(isp.mx0);
	free(isp.mx1);
	free(isp.mw);
	free(isp.my0);
	free(isp.my1);
	free(isp.mwy);

	return 0;
}
