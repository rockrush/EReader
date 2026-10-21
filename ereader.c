#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <limits.h>
#include <dirent.h>
#include <libfyaml.h>
#include <sys/stat.h>
#include <math.h>
#include <pthread.h>
#include <termios.h>

#include "draw.h"
#include "doc.h"

#define FBDEV		"/dev/fb0"
#define MARGIN		1
#define GRAY		128	/* 屏幕空白处的灰色填充 */
#define TRIM_MARGIN	8	/* 去白边后内容四周保留的边距（像素） */

struct ereader_cfg {
	uint16_t scr_w;
	uint16_t scr_h;
	uint8_t ui_h;
	int32_t rotate;
	struct termios term;
	char path[PATH_MAX];	// 当前处理路径
} cfg = {
	.scr_w = 800,
	.scr_h = 600,
	.ui_h = 0,
	.rotate = 270,	// 固定旋转 270 度
};

static uint16_t pdf_w, pdf_h;

static int zoom = 280;
static int cur_page = 1;	// 当前页码

static struct doc *doc;
static char *pbuf;		/* current page */
static int prows, pcols;	/* current page dimensions */
static int pcol;		/* page position */
static int scol;		/* screen position */
int bpp;			/* bytes per pixel */

void term_setup(struct termios *term);
void term_cleanup(struct termios *term);
void fm_set_view(uint16_t w, uint16_t h);
int show_text(const char *path, uint16_t w, uint16_t h);
int show_pdf(char *path, uint16_t w, uint16_t h);
int show_filemgr(char *path, uint16_t w, uint16_t h);

/* this can be optimised based on framebuffer pixel format */
void fb_set(char *d, unsigned r, unsigned g, unsigned b)
{
	unsigned c = fb_val(r, g, b);
	for (int i = 0; i < bpp; i++)
		d[i] = (c >> (i << 3)) & 0xff;
}

/* 在屏幕坐标设置一个像素（越界自动忽略） */
void px(int x, int y, unsigned r, unsigned g, unsigned b)
{
	char c[4];
	if (x < 0 || y < 0 || x >= cfg.scr_w || y >= cfg.scr_h)
		return;
	fb_set(c, r, g, b);
	memcpy(fb_mem(y) + (size_t) x * bpp, c, bpp);
}

/* 填充矩形（屏幕坐标，越界自动裁剪） */
void fill_rect(int x, int y, int w, int h, unsigned r, unsigned g, unsigned b)
{
	int i, j, x1 = x + w, y1 = y + h;
	char c[4];
	fb_set(c, r, g, b);
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	x1 = (x1 > cfg.scr_w) ? cfg.scr_w : x1;
	y1 = (y1 > cfg.scr_h) ? cfg.scr_h : y1;
	for (i = y; i < y1; i++)
		for (j = x; j < x1; j++)
			memcpy(fb_mem(i) + (size_t) j * bpp, c, bpp);
}

/* 填充圆 */
static void fill_circle(int cx, int cy, int rad, unsigned r, unsigned g, unsigned b)
{
	int x, y, rr = rad * rad;
	for (y = cy - rad; y <= cy + rad; y++)
		for (x = cx - rad; x <= cx + rad; x++)
			if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= rr)
				px(x, y, r, g, b);
}

/* 填充圆角矩形 */
static void fill_round_rect(int x, int y, int w, int h, int rad, unsigned r, unsigned g, unsigned b)
{
	if (w < 2 * rad)
		rad = w / 2;
	if (h < 2 * rad)
		rad = h / 2;
	if (rad < 1)
		rad = 1;
	fill_rect(x + rad, y, w - 2 * rad, h, r, g, b);
	fill_rect(x, y + rad, w, h - 2 * rad, r, g, b);
	fill_circle(x + rad, y + rad, rad, r, g, b);
	fill_circle(x + w - 1 - rad, y + rad, rad, r, g, b);
	fill_circle(x + rad, y + h - 1 - rad, rad, r, g, b);
	fill_circle(x + w - 1 - rad, y + h - 1 - rad, rad, r, g, b);
}

/* 画粗线（Bresenham，每点画一个小方块） */
static void draw_thick_line(int x0, int y0, int x1, int y1, int thick, unsigned color)
{
	int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
	int err = dx + dy, e2, h = thick / 2;
	while (1) {
		fill_rect(x0 - h, y0 - h, thick, thick, color, color, color);
		if (x0 == x1 && y0 == y1)
			break;
		e2 = 2 * err;
		if (e2 >= dy) {
			err += dy;
			x0 += sx;
		}
		if (e2 <= dx) {
			err += dx;
			y0 += sy;
		}
	}
}

/* 带边框的圆角按钮 */
static void draw_button(int x, int y, int w, int h)
{
	int rad = h / 5;
	int thick = MAX(1, h / 20);
	if (rad < 2)
		rad = 2;
	fill_round_rect(x, y, w, h, rad, UI_BORDER, UI_BORDER, UI_BORDER);
	fill_round_rect(x + thick, y + thick, w - 2 * thick, h - 2 * thick, MAX(1, rad - thick), UI_BG, UI_BG, UI_BG);
}

/* 左/右箭头：dir = -1 左，+1 右 */
static void draw_arrow(int cx, int cy, int w, int h, int dir)
{
	int thick = MAX(1, h / 10);
	int tip = cx + dir * w / 4;
	int base = cx - dir * w / 4;
	draw_thick_line(base, cy, tip, cy, thick, UI_ICON);
	draw_thick_line(tip, cy, tip - dir * w / 7, cy - h / 4, thick * 5 / 7, UI_ICON);
	draw_thick_line(tip, cy, tip - dir * w / 7, cy + h / 4, thick * 5 / 7, UI_ICON);
}

/* 画圆弧（a0 -> a1 弧度，屏幕坐标 y 向下；自动处理环绕） */
static void draw_arc(int cx, int cy, int r, float a0, float a1, int thick, unsigned cr, unsigned cg, unsigned cb)
{
	float a;
	if (a1 < a0)
		a1 += 2 * M_PI;
	for (a = a0; a <= a1; a += 0.03f) {
		int x = cx + (int) (r * cosf(a));
		int y = cy + (int) (r * sinf(a));
		fill_circle(x, y, thick, cr, cg, cb);
	}
}

static void draw_go(int cx, int cy, int w, int h, int8_t dir)	// dir：左为1，右为-1
{
	int thick = MAX(1, h / 10);
	int pad = thick * 3;
	draw_thick_line(cx, cy + dir * (h / 2 - pad), cx - w / 2 + pad * 2, cy - dir * (h / 2 - pad), thick, UI_ICON);
	draw_thick_line(cx, cy + dir * (h / 2 - pad), cx + w / 2 - pad * 2, cy - dir * (h / 2 - pad), thick, UI_ICON);
}

/* 刷新：顶部留缺口的圆环 + 末端箭头（顺时针） */
static void draw_refresh(int cx, int cy, int h)
{
	int thick = MAX(1, h / 16);
	int r = h * 3 / 8;
	float gap = 0.5f;
	float a0 = M_PI * 1.5f - gap;
	float a1 = M_PI * 1.5f + gap;
	int ex = cx + (int) (r * cosf(a0));
	int ey = cy + (int) (r * sinf(a0));
	draw_arc(cx, cy, r, a1, a0 + 2 * M_PI, thick, UI_ICON, UI_ICON, UI_ICON);
	draw_thick_line(ex, ey, ex - r / 2, ey, h / 12, UI_ICON);
}

/* 开关：顶部开口的圆环 + 贯穿缺口的竖线 */
static void draw_power(int cx, int cy, int h)
{
	int thick = MAX(1, h / 16);
	int r = h * 3 / 8;
	draw_arc(cx, cy, r, M_PI + 0.5f, 3 * M_PI - 0.5f, thick, UI_ICON, UI_ICON, UI_ICON);
	draw_thick_line(cx - r, cy, cx + h / 8, cy, h / 10, UI_ICON);
}

/* WiFi 图标：cy 为底部实心点位置，弧线朝上；connected 为 0 时只画断点 */
static void draw_wifi(int cx, int cy, int rad, int8_t level)
{
	int k, r, thick = MAX(1, rad / 10);

	if (level < 0)
		return;	// 无信号，不画图标
	fill_circle(cx, cy, thick, UI_ICON, UI_ICON, UI_ICON);
	for (k = 1; k <= 3; k++) {
		float a;
		unsigned col = (level >= k) ? UI_ICON : UI_ICON / 2;
		r = rad * k / 3;
		for (a = 0.6f; a <= M_PI - 0.6f; a += 0.05f) {
			int x = cx + (int) (r * cosf(a));
			int y = cy - (int) (r * sinf(a));
			fill_circle(x, y, thick, col, col, col);
		}
	}
}

/* 电量图标：x 为电池左端，cy 为纵向中心，pct 为电量百分比 */
static void draw_battery(int x, int cy, int w, int h, int pct)
{
	int y = cy - h / 2;
	int rad = h / 5;
	int thick = MAX(1, h / 12);
	int inset = thick + h / 10;
	int nub_w = MAX(2, w / 12);
	int fill_w;
	if (rad < 1)
		rad = 1;
	fill_round_rect(x, y, w, h, rad, UI_BORDER, UI_BORDER, UI_BORDER);
	fill_round_rect(x + thick, y + thick, w - 2 * thick, h - 2 * thick,
			MAX(1, rad - thick), UI_BG, UI_BG, UI_BG);
	fill_rect(x + w, cy - h / 4, nub_w, h / 2, UI_BORDER, UI_BORDER, UI_BORDER);
	fill_w = (w - 2 * inset) * pct / 100;
	if (fill_w < 0)
		fill_w = 0;
	if (fill_w > w - 2 * inset)
		fill_w = w - 2 * inset;
	fill_rect(x + inset, y + inset, fill_w, h - 2 * inset, UI_ICON, UI_ICON, UI_ICON);
}

/* 读取电池电量百分比，失败返回 100 */
static int read_battery_pct(void)
{
	DIR *dir;
	struct dirent *de;
	int pct = -1;
	dir = opendir("/sys/class/power_supply");
	if (!dir)
		return 100;
	while ((de = readdir(dir))) {
		char path[320];
		FILE *f;
		int v;
		if (de->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path),
			"/sys/class/power_supply/%s/capacity", de->d_name);
		f = fopen(path, "r");
		if (f) {
			if (fscanf(f, "%d", &v) == 1 && v >= 0 && v <= 100)
				pct = v;
			fclose(f);
			if (pct >= 0)
				break;
		}
	}
	closedir(dir);
	return pct >= 0 ? pct : 100;
}

static int8_t wifi_level(void) {
	char line[256];
	int link = 0, level = -100, noise = 0, line_no = 0;
	FILE *fp = fopen("/proc/net/wireless", "r");
	if (!fp) {
		perror("fopen /proc/net/wireless");
		return -1;
	}

	while (fgets(line, sizeof(line), fp)) {
		if (line_no++ < 2)	/* 前两行是表头，跳过 */
			continue;

		char *colon = strchr(line, ':');	/* 匹配接口名，行首格式如 "wlan0:" */
		if (!colon)
			continue;

		if (sscanf(colon + 1, "%d %d %d", &link, &level, &noise) >= 2)
			break;
	}
	fclose(fp);

	if ((level == 0) || (level == -100))	/* 某些驱动无信号时上报 0 */
		return -1;
	else if (level <= -80)
		return 0;
	else if (level <= -70)
		return 1;
	else if (level <= -60)
		return 2;
	else
		return 3;
}

/* 在空白区域（顶部行 gtop）绘制状态图标与按钮 */
void *draw_ui(void *arg)
{
	int *height = (int *)arg;
	int gtop = cfg.scr_h - *height;
	int gh, bar_h, bar_top, pad = 2;

	if (gtop < 0)
		gtop = 0;
	if (gtop >= cfg.scr_h)
		return NULL;			/* 页面填满屏幕，无空白区域 */
	gh = cfg.scr_h - gtop;
	if (gh < 32)
		return NULL;			/* 空白区域太小，不绘制 */

	bar_h = gh - pad * 2;
	bar_top = cfg.scr_h - bar_h - pad;

	while (true) {
		fill_rect(0, cfg.scr_h - cfg.ui_h, cfg.scr_w, cfg.ui_h, GRAY, GRAY, GRAY);
		// 从左往右
		int gap = 50;
		int width = bar_h * 2;
		for (int i = 0; i < 6; i++) {
			int x = gap / 2 + i * (gap + width);
			int cx = x + width / 2, cy = bar_top + bar_h / 2;
			int s = width / 3;
			if (s < 2)
				s = 2;
			draw_button(x, bar_top, width, bar_h);
			switch (i) {
			case 0: draw_arrow(cx, cy, width, bar_h, -1); break;	/* 上一页 */
			case 1: draw_arrow(cx, cy, width, bar_h, +1); break;	/* 下一页 */
			case 2: draw_go(cx, cy, width, bar_h, -1); break;/* 确认 */
			case 3: draw_go(cx, cy, width, bar_h, 1); break;/* 返回 */
			case 4: draw_refresh(cx, cy, bar_h); break;	/* 刷新 */
			case 5: draw_power(cx, cy, bar_h); break;	/* 开关 */
			}
		}

		/* 从右往左：WiFi + 电量，靠右 */
		int cy = bar_top + bar_h / 2;
		int bat_x = cfg.scr_w - width - gap;
		draw_battery(bat_x, cy, width, bar_h * 4 / 5, read_battery_pct());
		draw_wifi(bat_x - gap / 2 - bar_h, bar_top + bar_h * 4 / 5, bar_h * 3 / 5, wifi_level());
		sleep(2);
	}
	return NULL;
}

//===================================================
static void draw(void)
{
	int i, j;
	char gray[4];
	char *rbuf = malloc(cfg.scr_w * bpp);
	fb_set(gray, GRAY, GRAY, GRAY);
	for (i = 0; i < cfg.scr_h - cfg.ui_h; i++) {
		int cbeg = MAX(scol, pcol);
		int cend = MIN(scol + cfg.scr_w, pcol + pcols);
		for (j = 0; j < cfg.scr_w; j++)
			memcpy(rbuf + j * bpp, gray, bpp);
		if (i >= 0 && i < prows && cbeg < cend) {
			memcpy(rbuf + (cbeg - scol) * bpp,
				pbuf + (i * pcols + cbeg - pcol) * bpp,
				(cend - cbeg) * bpp);
		}
		memcpy(fb_mem(i), rbuf, cfg.scr_w * bpp);
	}
	free(rbuf);
}

/* 满宽渲染，保证去白边后仍有足够分辨率 */
static void fitpage(int p)
{
	int w, h;
	if (doc_size(doc, p, &w, &h))
		return;
	if ((cfg.rotate / 90) % 2) {	/* 固定 270 度：宽高互换（旋转后页面横向） */
		int t = w;
		w = h;
		h = t;
	}
	if (w <= 0 || h <= 0)
		return;
	zoom = cfg.scr_w * 100 / w;
	zoom = MIN(1000, MAX(1, zoom));
}

/* 检测页面内容（非纯白像素）的包围盒，返回是否找到内容 */
static int content_bbox(char *buf, int rows, int cols, int bpp, int *x0, int *y0, int *x1, int *y1)
{
	unsigned char white[4];
	int x, y;
	fb_set((char *) white, 255, 255, 255);
	*x0 = cols;
	*y0 = rows;
	*x1 = -1;
	*y1 = -1;
	for (y = 0; y < rows; y++) {
		unsigned char *row = (unsigned char *) buf + (size_t) y * cols * bpp;
		for (x = 0; x < cols; x++)
			if (memcmp(row + (size_t) x * bpp, white, bpp)) {
				if (x < *x0) *x0 = x;
				if (x > *x1) *x1 = x;
				if (y < *y0) *y0 = y;
				if (y > *y1) *y1 = y;
			}
	}
	return *x1 >= *x0 && *y1 >= *y0;
}

/* 裁剪缓冲区到内容包围盒，并保留 margin 边距 */
static char *crop_page(char *buf, int *rows, int *cols, int bpp, int x0, int y0, int x1, int y1, int margin)
{
	int nx = x0 - margin;
	int ny = y0 - margin;
	int nw = (x1 - x0 + 1) + 2 * margin;
	int nh = (y1 - y0 + 1) + 2 * margin;
	char *nbuf;
	int y;
	if (nx < 0) {
		nw += nx;
		nx = 0;
	}
	if (ny < 0) {
		nh += ny;
		ny = 0;
	}
	if (nx + nw > *cols)
		nw = *cols - nx;
	if (ny + nh > *rows)
		nh = *rows - ny;
	if (nw <= 0 || nh <= 0) {
		free(buf);
		return NULL;
	}
	nbuf = malloc((size_t) nw * nh * bpp);
	if (!nbuf) {
		free(buf);
		return NULL;
	}
	for (y = 0; y < nh; y++)
		memcpy(nbuf + (size_t) y * nw * bpp,
			buf + ((size_t) (ny + y) * *cols + nx) * bpp,
			(size_t) nw * bpp);
	free(buf);
	*rows = nh;
	*cols = nw;
	return nbuf;
}

/* 双线性缩放：把 sw×sh 源缓冲区拉伸到 dw×dh 目标缓冲区（帧缓冲字节序） */
static void scale_bilinear(char *src, int sw, int sh, char *dst, int dw, int dh, int bpp)
{
	int x, y;
	for (y = 0; y < dh; y++) {
		long sy = (long) (2 * y + 1) * sh * 65536 / (2 * dh) - 32768;
		int y0, y1, fy;
		if (sy < 0)
			sy = 0;
		if (sy > (long) (sh - 1) * 65536)
			sy = (long) (sh - 1) * 65536;
		y0 = (int) (sy >> 16);
		fy = (int) (sy & 0xffff);
		y1 = y0 + 1;
		if (y1 >= sh)
			y1 = y0;
		for (x = 0; x < dw; x++) {
			long sx = (long) (2 * x + 1) * sw * 65536 / (2 * dw) - 32768;
			int x0, x1, fx;
			unsigned r0, g0, b0, r1, g1, b1, r2, g2, b2, r3, g3, b3;
			if (sx < 0)
				sx = 0;
			if (sx > (long) (sw - 1) * 65536)
				sx = (long) (sw - 1) * 65536;
			x0 = (int) (sx >> 16);
			fx = (int) (sx & 0xffff);
			x1 = x0 + 1;
			if (x1 >= sw)
				x1 = x0;
			fb_get(src + ((long) y0 * sw + x0) * bpp, &r0, &g0, &b0);
			fb_get(src + ((long) y0 * sw + x1) * bpp, &r1, &g1, &b1);
			fb_get(src + ((long) y1 * sw + x0) * bpp, &r2, &g2, &b2);
			fb_get(src + ((long) y1 * sw + x1) * bpp, &r3, &g3, &b3);
			fb_set(dst + ((long) y * dw + x) * bpp,
				((long) r0 * (65536 - fx) * (65536 - fy)
				+ (long) r1 * fx * (65536 - fy)
				+ (long) r2 * (65536 - fx) * fy
				+ (long) r3 * fx * fy) >> 32,
				((long) g0 * (65536 - fx) * (65536 - fy)
				+ (long) g1 * fx * (65536 - fy)
				+ (long) g2 * (65536 - fx) * fy
				+ (long) g3 * fx * fy) >> 32,
				((long) b0 * (65536 - fx) * (65536 - fy)
				+ (long) b1 * fx * (65536 - fy)
				+ (long) b2 * (65536 - fx) * fy
				+ (long) b3 * fx * fy) >> 32);
		}
	}
}

/* 渲染第 p 页：去白边后等比例缩放（不变形）放入横向 A4 白纸，宽度不足处留白，下方留灰 */
static int renderpage(int p)
{
	char *buf;
	int cx0, cy0, cx1, cy1;
	int tw, th;
	char *dst;

	fitpage(p);
	buf = doc_draw(doc, p, zoom, cfg.rotate, bpp, &prows, &pcols);
	if (!buf)
		return 1;
	if (content_bbox(buf, prows, pcols, bpp, &cx0, &cy0, &cx1, &cy1))
		buf = crop_page(buf, &prows, &pcols, bpp, cx0, cy0, cx1, cy1, TRIM_MARGIN);
	if (!buf)
		return 1;

	tw = cfg.scr_w;
	th = cfg.scr_w * 210 / 297;
	if (th > cfg.scr_h) {	/* 屏幕较矮时按高度适配，左右留灰 */
		th = cfg.scr_h;
		tw = cfg.scr_h * 297 / 210;
	}
	dst = malloc((size_t) tw * th * bpp);
	if (!dst) {
		free(buf);
		return 1;
	}
	/* 白色 A4 纸张背景 */
	char white[4];
	int i, j;
	fb_set(white, 255, 255, 255);
	for (i = 0; i < th; i++) {
		char *row = dst + (size_t) i * tw * bpp;
		for (j = 0; j < tw; j++)
			memcpy(row + (size_t) j * bpp, white, bpp);
	}

	/* 内容等比例缩放（不变形）到纸张内最大，居中放置，宽度不足处留白 */
	int scale_pct = MIN(tw * 100 / pcols, th * 100 / prows);
	int cw, ch, ox, oy, y;
	char *tmp;
	if (scale_pct < 1)
		scale_pct = 1;
	cw = pcols * scale_pct / 100;
	ch = prows * scale_pct / 100;
	if (cw < 1)
		cw = 1;
	if (ch < 1)
		ch = 1;
	tmp = malloc((size_t) cw * ch * bpp);
	if (!tmp) {
		free(buf);
		free(dst);
		return 1;
	}
	scale_bilinear(buf, pcols, prows, tmp, cw, ch, bpp);
	free(buf);
	ox = (tw - cw) / 2;
	oy = (th - ch) / 2;
	for (y = 0; y < ch; y++)
		memcpy(dst + ((size_t) (oy + y) * tw + ox) * bpp,
			tmp + (size_t) y * cw * bpp, (size_t) cw * bpp);
	free(tmp);

	pbuf = dst;
	prows = th;
	pcols = tw;
	return 0;
}

static int loadpage(int p)
{
	if (p < 1 || p > doc_pages(doc))
		return 1;
	if (pbuf) {
		free(pbuf);
		pbuf = NULL;
	}
	prows = 0;
	pcols = 0;
	if (renderpage(p))
		return 1;
	pcol = -pcols / 2;	/* 水平居中 */
	cur_page = p;
	return 0;
}

static int reload(char *path)
{
	doc_close(doc);
	doc = doc_open(path);
	if (!doc || !doc_pages(doc)) {
		fprintf(stderr, "\ncannot open <%s>\n", path);
		return 1;
	}
	if (!loadpage(cur_page))
		draw();
	return 0;
}

int show_pdf(char *path, uint16_t w, uint16_t h)
{
	int c, retval = EXIT_FAILURE;
	if (!path || !strlen(path)) {	// 此时显示文件管理器
		fprintf(stderr, "no PDF file specified\n");
		goto fail_file;
	}
	pdf_w = h;
	pdf_h = w;
	doc = doc_open(path);
	if (!doc || !doc_pages(doc)) {
		fprintf(stderr, "cannot open <%s>\n", path);
		goto fail_file;
	}

	if ((cur_page < 1) || (cur_page > doc_pages(doc)))
		cur_page = 1;
	loadpage(cur_page);
	scol = -w / 2;
	draw();

	while ((c = readkey()) != -1) {
		printf("show_pdf: %d, %d, %d, %d\n", prows, pcols, pdf_w, pdf_h);
		switch (c) {
		case 'q':	// 退出
			goto fail_file;
		case 'e':	// 刷新
			if (reload(path))
				goto fail_file;
			break;
		case 'j':	// 下一页
			loadpage(cur_page + 1);
			break;
		case 'k':	// 上一页
			loadpage(cur_page - 1);
			break;
		default:
			continue;
		}
		scol = MAX(pcol - cfg.scr_w + MARGIN, MIN(pcol + pcols - MARGIN, scol));
		if (pcols <= cfg.scr_w)
			scol = pcol + (pcols - cfg.scr_w) / 2;	/* 页面比屏幕窄时水平居中 */
		draw();
	}
	retval = EXIT_SUCCESS;
fail_file:
	if (pbuf) {
		free(pbuf);
		pbuf = NULL;
	}
	if (doc) {
		doc_close(doc);
		doc = NULL;
	}
	return retval;
}
//===================================================

/* 粗略判断文本文件：前 4KB 无 NUL 字节，且至少九成是可打印字符 */
int is_text(const char *path)
{
	unsigned char buf[4096];
	int n, i, good = 0;
	FILE *fp = fopen(path, "rb");

	if (!fp)
		return 0;
	n = fread(buf, 1, sizeof(buf), fp);
	fclose(fp);
	if (n <= 0)
		return 1;		/* 空文件按文本处理 */
	for (i = 0; i < n; i++) {
		unsigned char c = buf[i];

		if (c == 0)
			return 0;
		if (c == '\n' || c == '\r' || c == '\t' ||
				(c >= 0x20 && c != 0x7f))
			good++;
	}
	return good * 10 >= n * 9;
}

int is_pdf(const char *name)
{
	const char *dot = strrchr(name, '.');
	return dot && dot != name && !strcasecmp(dot, ".pdf");
}

static void ebook(void) {
	pthread_t th_ui;
	struct stat st;
	char tmp_path[PATH_MAX];
	if (!realpath(cfg.path, tmp_path))
		strncpy(tmp_path, "/", 2);

	if (cfg.ui_h)
		if (pthread_create(&th_ui, NULL, draw_ui, &cfg.ui_h) != 0) {
			perror("creating UI thread");
			return;
		}

	fm_set_view(cfg.scr_w, cfg.scr_h - cfg.ui_h);
	while (true) {
		if (!tmp_path[0])
			strncpy(tmp_path, "/", 2);
		char *base = strrchr(tmp_path, '/');	// 定位文件名

		if (stat(tmp_path, &st) != 0)	// 读不了：不存在或没权限
			goto _parent;
		if (S_ISDIR(st.st_mode)) {
			show_filemgr(tmp_path, cfg.scr_w, cfg.scr_h - cfg.ui_h);
			goto _parent;
		} else if (!S_ISREG(st.st_mode))
			goto _parent;

		if (is_pdf(base)) {
			show_pdf(tmp_path, cfg.scr_w, cfg.scr_h - cfg.ui_h);
			goto _parent;
		}

		// TODO: 其他文件格式

		if (is_text(tmp_path))
			show_text(tmp_path, cfg.scr_w, cfg.scr_h - cfg.ui_h);
_parent:
		base[0] = 0;
	}
}

static void sigcont(int sig)	{term_setup(&cfg.term);}

int main(int argc, char *argv[])
{
	struct fy_document *fyd = fy_document_build_from_file(NULL, "./ereader.yml");
	if (!fyd) {
		fprintf(stderr, "cannot parse ereader.yml\n");
		return 1;
	}
	memset(cfg.path, 0, PATH_MAX);
	int count = fy_document_scanf(fyd, "/path %s /page %u", cfg.path, &cur_page);
	if (count != 2) {
		fy_document_destroy(fyd);
		return 1;
	}
	fy_document_destroy(fyd);

	if (fb_init(FBDEV))
		return 1;
	cfg.scr_h = fb_rows();
	cfg.scr_w = fb_cols();
	cfg.ui_h = cfg.scr_h - cfg.scr_w * 210 / 297 - 1;
	if (cfg.ui_h < 32)
		cfg.ui_h = 0;
	bpp = FBM_BPP(fb_mode());

	term_setup(&cfg.term);
	signal(SIGCONT, sigcont);
	ebook();
	term_cleanup(&cfg.term);

	fb_free();
	return 0;
}
