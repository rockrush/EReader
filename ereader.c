#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <pthread.h>
#include <libfyaml.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include "draw.h"
#include "doc.h"

#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))

#define FBDEV		"/dev/fb0"
#define MARGIN		1
#define GRAY		128	/* 屏幕空白处的灰色填充 */
#define TRIM_MARGIN	8	/* 去白边后内容四周保留的边距（像素） */
#define UI_BG		235	/* 按钮/纸张填充色 */
#define UI_BORDER	90	/* 按钮/电池边框色 */
#define UI_ICON		45	/* 图标/文字颜色 */

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

static int zoom = 280;
static int num = 1;		/* page number */

static struct doc *doc;
static char *pbuf;		/* current page */
static int prows, pcols;	/* current page dimensions */
static int prow, pcol;		/* page position */
static int srow, scol;		/* screen position */
static int bpp;			/* bytes per pixel */
static int count;

/* 在屏幕坐标设置一个像素（越界自动忽略） */
static void px(int x, int y, unsigned r, unsigned g, unsigned b)
{
	char c[4];
	if (x < 0 || y < 0 || x >= cfg.scr_w || y >= cfg.scr_h)
		return;
	fb_set(c, r, g, b);
	memcpy(fb_mem(y) + (size_t) x * bpp, c, bpp);
}

/* 填充矩形（屏幕坐标，越界自动裁剪） */
static void fill_rect(int x, int y, int w, int h, unsigned r, unsigned g, unsigned b)
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

static void draw(void)
{
	int i, j;
	char gray[4];
	char *rbuf = malloc(cfg.scr_w * bpp);
	fb_set(gray, GRAY, GRAY, GRAY);
	for (i = srow; i < srow + cfg.scr_h - cfg.ui_h; i++) {
		int cbeg = MAX(scol, pcol);
		int cend = MIN(scol + cfg.scr_w, pcol + pcols);
		for (j = 0; j < cfg.scr_w; j++)
			memcpy(rbuf + j * bpp, gray, bpp);
		if (i >= prow && i < prow + prows && cbeg < cend) {
			memcpy(rbuf + (cbeg - scol) * bpp,
				pbuf + ((i - prow) * pcols + cbeg - pcol) * bpp,
				(cend - cbeg) * bpp);
		}
		memcpy(fb_mem(i - srow), rbuf, cfg.scr_w * bpp);
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
static int content_bbox(char *buf, int rows, int cols, int bpp,
		int *x0, int *y0, int *x1, int *y1)
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
static void scale_bilinear(char *src, int sw, int sh, char *dst, int dw, int dh,
		int bpp)
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
	{
		char white[4];
		int i, j;
		fb_set(white, 255, 255, 255);
		for (i = 0; i < th; i++) {
			char *row = dst + (size_t) i * tw * bpp;
			for (j = 0; j < tw; j++)
				memcpy(row + (size_t) j * bpp, white, bpp);
		}
	}
	/* 内容等比例缩放（不变形）到纸张内最大，居中放置，宽度不足处留白 */
	{
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
	}
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
	prow = 0;		/* 顶部对齐，下方以灰色填充 */
	pcol = -pcols / 2;	/* 水平居中 */
	num = p;
	return 0;
}

static int readkey(void)
{
	unsigned char b;
	if (read(0, &b, 1) <= 0)
		return -1;
	return b;
}

static int getcount(int def)
{
	int result = count ? count : def;
	count = 0;
	return result;
}

static int reload(void)
{
	doc_close(doc);
	doc = doc_open(cfg.path);
	if (!doc || !doc_pages(doc)) {
		fprintf(stderr, "\ncannot open <%s>\n", cfg.path);
		return 1;
	}
	if (!loadpage(num))
		draw();
	return 0;
}

/* this can be optimised based on framebuffer pixel format */
void fb_set(char *d, unsigned r, unsigned g, unsigned b)
{
	unsigned c = fb_val(r, g, b);
	int i;
	for (i = 0; i < bpp; i++)
		d[i] = (c >> (i << 3)) & 0xff;
}

static int show_pdf(char *path)
{
	int c, retval = EXIT_FAILURE;
	printf("show_pdf: %s\n", path);
	if (!path || !strlen(path)) {	// 此时显示文件管理器
		fprintf(stderr, "no PDF file specified\n");
		goto fail_file;
	}
	doc = doc_open(path);
	if (!doc || !doc_pages(doc)) {
		fprintf(stderr, "cannot open <%s>\n", path);
		goto fail_file;
	}
	/* 供 e 键刷新使用；从文件管理器打开时路径可能与启动配置不同 */
	strncpy(cfg.path, path, sizeof(cfg.path) - 1);
	cfg.path[sizeof(cfg.path) - 1] = 0;
	if (num < 1 || num > doc_pages(doc))
		num = 1;
	loadpage(num);
	srow = prow;
	scol = -cfg.scr_w / 2;
	draw();

	while ((c = readkey()) != -1) {
		switch (c) {
		case 'q':	// 退出
			goto fail_file;
		case 'e':	// 刷新
			if (reload())
				goto fail_file;
			break;
		case 'j':	// 下一页
			if (!loadpage(num + getcount(1)))
				srow = prow;
			break;
		case 'k':	// 上一页
			if (!loadpage(num - getcount(1)))
				srow = prow;
			break;
		default:
			continue;
		}
		srow = MAX(prow - cfg.scr_h + MARGIN, MIN(prow + prows - MARGIN, srow));
		if (prows <= cfg.scr_h)
			srow = prow;	/* 页面比屏幕矮时顶部对齐 */
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

struct fm_entry {
	char name[NAME_MAX + 1];
	int isdir;
	int isreg;
};

struct fm_view {
	char path[PATH_MAX];
	struct fm_entry *ents;
	int n;
	int sel;
	int top;
};

#define FM_FONT_PX	10	/* 基准字号（像素），按屏幕缩放倍数放大 */
#define FM_FIRST	32
#define FM_LAST		126

struct fm_glyph {
	int w, h;		/* 位图尺寸 */
	int left, top;		/* 位图相对基线的偏移 */
	int advance;		/* 步进（像素） */
	unsigned char *bitmap;	/* 8 位灰度位图，行宽为 w */
	int loaded;
};

/* 优先使用等宽字体；也可用环境变量 EREADER_FONT 指定字体文件 */
static const char *fm_font_paths[] = {
	"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
	"/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
	"/usr/share/fonts/TTF/DejaVuSansMono.ttf",
	"/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
	"/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
	"/usr/share/fonts/truetype/freefont/FreeMono.ttf",
	"/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
	"/usr/share/fonts/truetype/hack/Hack-Regular.ttf",
	"/usr/share/fonts/truetype/droid/DroidSansMono.ttf",
	"/system/fonts/DroidSansMono.ttf",
	NULL,
};

static FT_Library fm_lib;
static FT_Face fm_face;
static struct fm_glyph fm_glyphs[FM_LAST - FM_FIRST + 1];
static int fm_px;		/* 当前字号（像素） */
static int fm_ascent;		/* 基线到行顶的距离 */
static int fm_line_height;	/* 行高 */

/* 文件管理器界面逆时针旋转 90 度：逻辑宽高与屏幕宽高互换 */
static int fm_lw(void)	{return cfg.scr_h - cfg.ui_h;}
static int fm_lh(void)	{return cfg.scr_w;}

static int fm_scale(void)
{
	int s = MIN(fm_lw() / 400, fm_lh() / 240);
	return MIN(3, MAX(1, s));
}

/* 逻辑坐标下的矩形：逆时针 90 度后映射到屏幕 */
static void fm_fill_rect(int x, int y, int w, int h, unsigned r, unsigned g, unsigned b)
{
	fill_rect(y, fm_lw() - x - w, h, w, r, g, b);
}

static void fm_font_close(void)
{
	int i;

	for (i = 0; i < FM_LAST - FM_FIRST + 1; i++) {
		free(fm_glyphs[i].bitmap);
		memset(&fm_glyphs[i], 0, sizeof(fm_glyphs[i]));
	}
	if (fm_face) {
		FT_Done_Face(fm_face);
		fm_face = NULL;
	}
	if (fm_lib) {
		FT_Done_FreeType(fm_lib);
		fm_lib = NULL;
	}
	fm_px = 0;
	fm_ascent = 0;
	fm_line_height = 0;
}

static int fm_font_load(void)
{
	const char *env, *path = NULL;
	int px = FM_FONT_PX * fm_scale();
	int i;

	if (fm_face && px == fm_px)
		return 0;
	fm_font_close();
	if (FT_Init_FreeType(&fm_lib))
		return 1;
	fm_face = NULL;
	env = getenv("EREADER_FONT");
	if (env && env[0]) {
		if (!FT_New_Face(fm_lib, env, 0, &fm_face))
			path = env;
	} else {
		for (i = 0; fm_font_paths[i]; i++)
			if (!FT_New_Face(fm_lib, fm_font_paths[i], 0, &fm_face)) {
				path = fm_font_paths[i];
				break;
			}
	}
	if (!path || !fm_face ||
			FT_Set_Pixel_Sizes(fm_face, 0, px)) {
		fprintf(stderr, "cannot load font\n");
		fm_font_close();
		return 1;
	}
	fm_px = px;
	fm_ascent = (fm_face->size->metrics.ascender + 63) >> 6;
	fm_line_height = (fm_face->size->metrics.height + 63) >> 6;
	return 0;
}

/* 字体由文件管理器和文本阅读器共用，最后一个使用者退出时才释放 */
static int fm_font_users;

static int fm_font_acquire(void)
{
	if (fm_font_load())
		return 1;
	fm_font_users++;
	return 0;
}

static void fm_font_release(void)
{
	if (fm_font_users > 0 && --fm_font_users == 0)
		fm_font_close();
}

/* 惰性缓存 ASCII 字形，按当前字号渲染一次 */
static struct fm_glyph *fm_glyph_get(unsigned char c)
{
	struct fm_glyph *gl;
	FT_GlyphSlot slot;
	FT_Bitmap *bm;
	int row;

	if (c < FM_FIRST || c > FM_LAST)
		c = '?';
	gl = &fm_glyphs[c - FM_FIRST];
	if (gl->loaded)
		return gl;
	gl->loaded = 1;
	if (!fm_face || FT_Load_Char(fm_face, c, FT_LOAD_RENDER))
		return gl;
	slot = fm_face->glyph;
	bm = &slot->bitmap;
	gl->w = (int) bm->width;
	gl->h = (int) bm->rows;
	gl->left = slot->bitmap_left;
	gl->top = slot->bitmap_top;
	gl->advance = (slot->advance.x + 32) >> 6;
	if (!gl->w || !gl->h)
		return gl;
	gl->bitmap = malloc((size_t) gl->w * gl->h);
	if (!gl->bitmap) {
		gl->w = 0;
		gl->h = 0;
		return gl;
	}
	for (row = 0; row < gl->h; row++) {
		long off = bm->pitch >= 0 ? (long) row * bm->pitch
			: (long) (gl->h - 1 - row) * -bm->pitch;
		memcpy(gl->bitmap + (size_t) row * gl->w, bm->buffer + off, gl->w);
	}
	return gl;
}

/* 单个字符的步进（越界字符按 '?' 处理） */
static int fm_advance(unsigned char c)
{
	int adv = fm_glyph_get(c)->advance;

	return adv > 0 ? adv : 1;
}

/* 按字形灰度覆盖值把前景色混合到帧缓冲（逻辑坐标，绘制时转 90 度） */
static void fm_blend_px(int x, int y, int a, unsigned r, unsigned g, unsigned b)
{
	char *d;
	unsigned br, bg, bb;
	int sx = y, sy = fm_lw() - 1 - x;	/* 屏幕坐标 */

	if (x < 0 || y < 0 || x >= fm_lw() || y >= fm_lh())
		return;
	if (a >= 255) {
		px(sx, sy, r, g, b);
		return;
	}
	d = fb_mem(sy) + (size_t) sx * bpp;
	fb_get(d, &br, &bg, &bb);
	px(sx, sy, (r * a + br * (255 - a)) / 255,
		(g * a + bg * (255 - a)) / 255,
		(b * a + bb * (255 - a)) / 255);
}

/* y 为文字行顶部，返回绘制后的横向位置 */
static int fm_char(int x, int y, unsigned char c,
		unsigned r, unsigned g, unsigned b)
{
	struct fm_glyph *gl = fm_glyph_get(c);
	int bx = x + gl->left;
	int by = y + fm_ascent - gl->top;
	int row, col;

	for (row = 0; gl->bitmap && row < gl->h; row++) {
		const unsigned char *src = gl->bitmap + (size_t) row * gl->w;
		for (col = 0; col < gl->w; col++)
			if (src[col])
				fm_blend_px(bx + col, by + row, src[col], r, g, b);
	}
	return gl->advance;
}

static int fm_text(int x, int y, const char *s,
		unsigned r, unsigned g, unsigned b)
{
	while (*s) {
		unsigned char c = (unsigned char) *s++;
		if (c >= 0x80) {
			while (((unsigned char) *s & 0xc0) == 0x80)
				s++;
			c = '?';
		}
		x += fm_char(x, y, c, r, g, b);
	}
	return x;
}

#define TX_MAX_SIZE	(32 << 20)	/* 文本文件读取上限 */
#define TX_MAX_LINES	(1 << 20)	/* 折行后的显示行上限 */
#define TX_TAB		8		/* tab 展开后的列数 */

struct tx_line {
	size_t off;		/* 在文本缓冲区中的偏移 */
	size_t len;		/* 字节长度（不含换行） */
};

struct tx_view {
	char *text;
	size_t size;
	struct tx_line *lines;	/* 折行后的显示行 */
	int n;
	int top;		/* 顶部显示行号 */
};

/* 载入文本：展开 tab，UTF-8 与控制字符换成可显示的单字节 */
static char *tx_load(const char *path, size_t *size)
{
	FILE *fp;
	char *in, *out;
	size_t len, n = 0, i, tabs = 0;
	long sz;

	fp = fopen(path, "r");
	if (!fp)
		return NULL;
	if (fseek(fp, 0, SEEK_END) || (sz = ftell(fp)) < 0 ||
			fseek(fp, 0, SEEK_SET)) {
		fclose(fp);
		return NULL;
	}
	len = (size_t) sz > TX_MAX_SIZE ? TX_MAX_SIZE : (size_t) sz;
	if ((size_t) sz > len)
		fprintf(stderr, "%s: only the first %d MB are shown\n", path,
			TX_MAX_SIZE >> 20);
	in = malloc(len + 1);
	if (!in) {
		fclose(fp);
		return NULL;
	}
	len = fread(in, 1, len, fp);
	fclose(fp);
	in[len] = 0;

	for (i = 0; i < len; i++)
		if (in[i] == '\t')
			tabs++;
	out = malloc(len + tabs * (TX_TAB - 1) + 1);
	if (!out) {
		free(in);
		return NULL;
	}
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char) in[i];
		if (c == '\t') {
			size_t stop = (n / TX_TAB + 1) * TX_TAB;
			while (n < stop)
				out[n++] = ' ';
		} else if (c == '\r') {
			if (i + 1 < len && in[i + 1] == '\n')
				continue;	/* CRLF 只保留 LF */
			out[n++] = '\n';
		} else if (c >= 0x80) {
			while (i + 1 < len && ((unsigned char) in[i + 1] & 0xc0) == 0x80)
				i++;
			out[n++] = '?';
		} else if (c < 0x20 && c != '\n') {
			out[n++] = ' ';
		} else if (c == 0x7f) {
			out[n++] = ' ';
		} else {
			out[n++] = (char) c;
		}
	}
	out[n] = 0;
	free(in);
	*size = n;
	return out;
}

static int tx_push(struct tx_view *v, int *cap, size_t off, size_t len)
{
	if (v->n == *cap) {
		int ncap = *cap ? *cap * 2 : 256;
		struct tx_line *tmp = realloc(v->lines,
			(size_t) ncap * sizeof(*tmp));

		if (!tmp)
			return 1;
		v->lines = tmp;
		*cap = ncap;
	}
	v->lines[v->n].off = off;
	v->lines[v->n].len = len;
	v->n++;
	return 0;
}

/* 返回 s 前 len 字节中能放进 avail 像素宽度的字节数（优先在空格后断开） */
static size_t tx_fit(const char *s, size_t len, int avail)
{
	size_t i, last = 0;
	int w = 0;

	for (i = 0; i < len; i++) {
		int cw = fm_advance((unsigned char) s[i]);
		if (i > 0 && w + cw > avail)
			return last ? last : i;
		w += cw;
		if (s[i] == ' ')
			last = i + 1;
	}
	return len;
}

/* 把文本按可用宽度切成显示行 */
static int tx_format(struct tx_view *v, int avail)
{
	size_t pos = 0;
	int cap = 0;

	if (avail < 1)
		avail = 1;
	while (pos <= v->size && v->n < TX_MAX_LINES) {
		size_t end = pos;

		while (end < v->size && v->text[end] != '\n')
			end++;
		if (end == pos && tx_push(v, &cap, pos, 0))
			return 1;
		while (pos < end && v->n < TX_MAX_LINES) {
			size_t len = tx_fit(v->text + pos, end - pos, avail);

			if (len < 1)
				len = 1;
			if (tx_push(v, &cap, pos, len))
				return 1;
			pos += len;
		}
		if (end >= v->size)
			break;
		pos = end + 1;
	}
	return 0;
}

/* 绘制 len 字节的文本（调用前已清理为 ASCII），返回绘制后的横向位置 */
static int tx_text(int x, int y, const char *s, size_t len, unsigned r, unsigned g, unsigned b)
{
	for (size_t i = 0; i < len; i++)
		x += fm_char(x, y, (unsigned char) s[i], r, g, b);
	return x;
}

static int tx_line_h(void)
{
	return MAX(1, fm_line_height) + 2 * fm_scale();
}

static int tx_visible(void)
{
	int visible = (fm_lh() - 12 * fm_scale()) / tx_line_h();

	return MAX(1, visible);
}

static void tx_draw(struct tx_view *v)
{
	int pad = 6 * fm_scale();
	int line_h = tx_line_h();
	int visible = tx_visible();
	int i;

	fm_fill_rect(0, 0, fm_lw(), fm_lh(), 255, 255, 255);
	for (i = 0; i < visible && v->top + i < v->n; i++) {
		const struct tx_line *ln = &v->lines[v->top + i];

		tx_text(pad, pad + i * line_h, v->text + ln->off, ln->len, UI_ICON, UI_ICON, UI_ICON);
	}
}

/* 文本阅读器：j/k 上下翻半屏，q 退出 */
static int show_text(const char *path)
{
	struct tx_view v;
	int c, step, visible, maxtop, retval = EXIT_FAILURE;

	memset(&v, 0, sizeof(v));
	if (fm_font_acquire())
		return EXIT_FAILURE;
	v.text = tx_load(path, &v.size);
	if (!v.text) {
		fprintf(stderr, "cannot open <%s>\n", path);
		goto out;
	}
	if (tx_format(&v, fm_lw() - 12 * fm_scale()))
		goto out;
	visible = tx_visible();
	maxtop = MAX(0, v.n - visible);
	tx_draw(&v);
	while ((c = readkey()) != -1) {
		switch (c) {
		case 'q':	// 退出
			retval = EXIT_SUCCESS;
			goto out;
		case 'j':	// 向下翻半屏
			step = MAX(1, visible / 2);
			v.top = MIN(maxtop, v.top + step);
			break;
		case 'k':	// 向上翻半屏
			step = MAX(1, visible / 2);
			v.top = MAX(0, v.top - step);
			break;
		default:
			continue;
		}
		tx_draw(&v);
	}
	retval = EXIT_SUCCESS;
out:
	free(v.lines);
	free(v.text);
	fm_font_release();
	return retval;
}

static int fm_cmp(const void *a, const void *b)
{
	const struct fm_entry *ea = a;
	const struct fm_entry *eb = b;
	int d;

	if (ea->isdir != eb->isdir)
		return ea->isdir ? -1 : 1;
	d = strcasecmp(ea->name, eb->name);
	return d ? d : strcmp(ea->name, eb->name);
}

static int fm_scan(const char *path, struct fm_entry **out, int *nout)
{
	struct fm_entry *ents = NULL;
	struct dirent *de;
	int n = 0, cap = 0;
	DIR *dir = opendir(path);

	if (!dir)
		return 1;
	while ((de = readdir(dir))) {
		char full[PATH_MAX];
		struct fm_entry *ent;
		struct stat st;
		size_t len;

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (snprintf(full, sizeof(full), "%s%s%s", path,
				strcmp(path, "/") ? "/" : "", de->d_name) >= (int) sizeof(full))
			continue;
		if (stat(full, &st))
			continue;
		if (n == cap) {
			int ncap = cap ? cap * 2 : 64;
			struct fm_entry *tmp = realloc(ents, (size_t) ncap * sizeof(*tmp));
			if (!tmp) {
				free(ents);
				closedir(dir);
				return 1;
			}
			ents = tmp;
			cap = ncap;
		}
		ent = &ents[n++];
		len = strlen(de->d_name);
		if (len > sizeof(ent->name) - 1)
			len = sizeof(ent->name) - 1;
		memcpy(ent->name, de->d_name, len);
		ent->name[len] = 0;
		ent->isdir = S_ISDIR(st.st_mode);
		ent->isreg = S_ISREG(st.st_mode);
	}
	closedir(dir);
	if (n > 1)
		qsort(ents, n, sizeof(*ents), fm_cmp);
	*out = ents;
	*nout = n;
	return 0;
}

static void fm_trim_slash(char *path)
{
	size_t len = strlen(path);
	while (len > 1 && path[len - 1] == '/')
		path[--len] = 0;
}

static int fm_chdir(struct fm_view *v, const char *path, const char *select)
{
	struct fm_entry *ents;
	int n, i;

	if (fm_scan(path, &ents, &n))
		return 1;
	free(v->ents);
	v->ents = ents;
	v->n = n;
	strncpy(v->path, path, sizeof(v->path) - 1);
	v->path[sizeof(v->path) - 1] = 0;
	v->sel = 0;
	v->top = 0;
	if (select) {
		for (i = 0; i < n; i++)
			if (!strcmp(v->ents[i].name, select)) {
				v->sel = i;
				break;
			}
	}
	return 0;
}

/* ".pdf" 后缀（不区分大小写），".pdf" 这类隐藏文件不算 */
static int fm_is_pdf(const char *name)
{
	const char *dot = strrchr(name, '.');
	return dot && dot != name && !strcasecmp(dot, ".pdf");
}

/* 粗略判断文本文件：前 4KB 无 NUL 字节，且至少九成是可打印字符 */
static int fm_is_text(const char *path)
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

static int fm_enter(struct fm_view *v)
{
	char next[PATH_MAX];
	int n;

	if (v->n <= 0)
		return 1;
	n = snprintf(next, sizeof(next), "%s%s%s", v->path,
			strcmp(v->path, "/") ? "/" : "", v->ents[v->sel].name);
	if (n < 0 || n >= (int) sizeof(next))
		return 1;
	if (v->ents[v->sel].isdir) {
		if (fm_chdir(v, next, NULL)) {
			fprintf(stderr, "cannot open <%s>\n", next);
			return 1;
		}
		return 0;
	}
	if (!v->ents[v->sel].isreg)
		return 1;	/* 设备、管道等不做处理 */
	if (fm_is_pdf(v->ents[v->sel].name)) {
		show_pdf(next);	/* 打开失败时 show_pdf 自己会报错 */
		return 0;
	}
	if (!fm_is_text(next))
		return 1;
	show_text(next);
	return 0;		/* 返回后重绘文件列表 */
}

static int fm_parent(struct fm_view *v)
{
	char parent[PATH_MAX];
	char child[NAME_MAX + 1];
	char *base;

	if (!strcmp(v->path, "/"))
		return 1;
	strncpy(parent, v->path, sizeof(parent) - 1);
	parent[sizeof(parent) - 1] = 0;
	base = strrchr(parent, '/');
	child[0] = 0;
	if (base) {
		strncpy(child, base + 1, sizeof(child) - 1);
		child[sizeof(child) - 1] = 0;
		if (base == parent)
			parent[1] = 0;
		else
			*base = 0;
	}
	if (fm_chdir(v, parent, child)) {
		fprintf(stderr, "cannot open <%s>\n", parent);
		return 1;
	}
	return 0;
}

static void fm_draw(struct fm_view *v)
{
	int scale = fm_scale();
	int pad = 6 * scale;
	int lw = fm_lw(), lh = fm_lh();
	int line_h = MAX(1, fm_line_height) + 2 * scale;
	int header_h = MAX(1, fm_line_height) + 2 * pad;
	int y0 = header_h + pad / 2;
	int visible = (lh - y0 - pad) / line_h;
	int x0 = cfg.ui_h;	/* 逆时针旋转后状态栏落在左侧，文字让开 */
	int i;

	if (visible < 1)
		visible = 1;
	if (v->sel < v->top)
		v->top = v->sel;
	if (v->sel >= v->top + visible)
		v->top = v->sel - visible + 1;
	if (v->top > v->n - visible)
		v->top = v->n - visible;
	if (v->top < 0)
		v->top = 0;

	fm_fill_rect(0, 0, lw, lh, UI_BG, UI_BG, UI_BG);
	//if (cfg.ui_h)
	//	fm_fill_rect(0, 0, x0, lh, GRAY, GRAY, GRAY);
	fm_text(x0 + pad, pad, v->path, UI_ICON, UI_ICON, UI_ICON);
	fm_fill_rect(x0, header_h, lw - x0, MAX(1, scale), UI_BORDER, UI_BORDER, UI_BORDER);

	if (!v->n) {
		fm_text(x0 + pad, y0 + scale, "(empty)", UI_BORDER, UI_BORDER, UI_BORDER);
	} else {
		for (i = v->top; i < v->n && i < v->top + visible; i++) {
			const struct fm_entry *e = &v->ents[i];
			int y = y0 + (i - v->top) * line_h;
			unsigned fg = (i == v->sel) ? UI_BG : UI_ICON;
			unsigned bg = (i == v->sel) ? UI_ICON : UI_BG;

			int x;
			if (i == v->sel)
				fm_fill_rect(x0, y, lw - x0, line_h, bg, bg, bg);
			x = fm_text(x0 + pad, y + scale, e->name, fg, fg, fg);
			if (e->isdir)
				fm_char(x, y + scale, '/', fg, fg, fg);
		}
	}
}

static int show_filemgr(char *path)
{
	struct fm_view v;
	struct stat st;
	int c, retval = EXIT_FAILURE;

	if (!path || !path[0])
		return EXIT_FAILURE;
	if (stat(path, &st) != 0)
		return EXIT_FAILURE;
	if (S_ISREG(st.st_mode))
		return EXIT_FAILURE;

	memset(&v, 0, sizeof(v));
	if (fm_font_acquire())
		return EXIT_FAILURE;

	fm_trim_slash(path);
	if (fm_chdir(&v, path, NULL)) {
		fprintf(stderr, "cannot open <%s>\n", path);
		goto out;
	}
	fm_draw(&v);
	while ((c = readkey()) != -1) {
		switch (c) {
		case 'q':
			if (!fm_parent(&v))
				fm_draw(&v);
			break;
		case 'e':
			if (!fm_enter(&v))
				fm_draw(&v);
			break;
		case 'j':
			if (v.sel + 1 < v.n) {
				v.sel++;
				fm_draw(&v);
			}
			break;
		case 'k':
			if (v.sel > 0) {
				v.sel--;
				fm_draw(&v);
			}
			break;
		default:
			continue;
		}
	}
	retval = EXIT_SUCCESS;
out:
	free(v.ents);
	fm_font_release();
	return retval;
}

static void ebook(void) {
	pthread_t th_ui;
	char tmp_path[PATH_MAX];
	if (!realpath(cfg.path, tmp_path))
		strncpy(tmp_path, "/", 2);

	if (pthread_create(&th_ui, NULL, draw_ui, &cfg.ui_h) != 0) {
		perror("creating UI thread");
		return;
	}

	while (true) {
		if (!tmp_path[0])
			strncpy(tmp_path, "/", 2);

		char *base = strrchr(tmp_path, '/');	// 定位文件名
		base = base ? base + 1 : tmp_path;
		char *dot = strrchr(base, '.');	// 只在该段内找最后一个点

		if (dot) {	// 有扩展名
			if (!strncmp(dot + 1, "pdf", 3)) {
				if (show_pdf(cfg.path) != EXIT_SUCCESS)
					dot[0] = 0;	// 退回文件所在目录
				continue;
			}
		}
		if (fm_is_text(tmp_path) && show_text(tmp_path) != EXIT_SUCCESS) {
			base[0] = 0;
			continue;
		}
		if (show_filemgr(tmp_path) != EXIT_SUCCESS)
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
	int count = fy_document_scanf(fyd, "/path %s /page %u", cfg.path, &num);
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
