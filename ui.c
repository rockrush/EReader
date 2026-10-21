#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <dirent.h>

#include "ereader.h"
#include "draw.h"

extern int bpp;

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
