#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <dirent.h>

#include "ereader.h"
#include "draw.h"
#include "font.h"

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
	int _rad = (rad < 1) ? 1 : ((h < 2 * rad) ? h / 2 : rad);
	if (w < 2 * _rad)
		_rad = w / 2;

	fill_rect(x + rad, y, w - 2 * rad, h, r, g, b);
	fill_rect(x, y + rad, w, h - 2 * rad, r, g, b);
	fill_circle(x + rad, y + rad, rad, r, g, b);
	fill_circle(x + w - rad, y + rad, rad, r, g, b);
	fill_circle(x + rad, y + h - rad, rad, r, g, b);
	fill_circle(x + w - rad, y + h - rad, rad, r, g, b);
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

/* 带边框的圆角按钮: 起点的屏幕横、纵坐标，按钮宽度、高度 */
static void draw_button(int x, int y, int w, int h)
{
	int rad = (w > 10) ? h / 5 : 2;
	int thick = w / 15;
	fill_round_rect(x, y, h, w, rad, UI_BORDER, UI_BORDER, UI_BORDER);
	fill_round_rect(x + thick, y + thick, h - 2 * thick, w - 2 * thick, MAX(1, rad - thick), UI_BG, UI_BG, UI_BG);
}

/* 左/右箭头：dir = -1 左，+1 右 */
static void draw_arrow(int x, int y, int w, int h, int dir)
{
	int x_pad = h / 4;	// 横向留空
	int cy = y + w / 2;	// 按钮中线纵坐标
	int thick = MAX(1, w / 12);	// 线条粗细
	int tip  = x + x_pad + ((dir > 0) ? 0 : (h / 2));	// 箭头横坐标
	int base = x + x_pad + ((dir < 0) ? 0 : (h / 2));	// 起点横坐标

	draw_thick_line(base, cy, tip, cy, thick, UI_ICON);
	draw_thick_line(tip, cy, tip + dir * h / 5, cy - w / 4, thick * 5 / 7, UI_ICON);
	draw_thick_line(tip, cy, tip + dir * h / 5, cy + w / 4, thick * 5 / 7, UI_ICON);
}

static void draw_go(int x, int y, int w, int h, int8_t dir)	// dir：左为1，右为-1
{
	int thick = MAX(1, w / 15);	// 线条粗细
	int x_pad = h / 3, y_pad = w / 3;
	int ys = (dir > 0) ? (y + y_pad) : (y + w - y_pad);
	int ye = (dir > 0) ? (y + w - y_pad) : (y + y_pad);

	draw_thick_line(x + x_pad, ys, x + h / 2, ye, thick, UI_ICON);
	draw_thick_line(x + h / 2, ye, x + h - x_pad, ys, thick, UI_ICON);
}

/* 刷新：顶部留缺口的圆环 + 末端箭头（顺时针） */
static void draw_refresh(int x, int y, int w, int h)
{
	int thick = MAX(1, w / 27);
	int r = w / 4;
	float a0 = M_PI * 1.5f - 0.5f;
	float a1 = M_PI * 1.5f + 0.5f;
	int ex = x + h / 2 + (int) (r * cosf(a0));
	int ey = y + w / 2 + (int) (r * sinf(a0));
	draw_arc(x + h / 2, y + w / 2, r, a1, a0 + 2 * M_PI, thick, UI_ICON, UI_ICON, UI_ICON);
	draw_thick_line(ex, ey, ex - r / 2, ey, w / 12, UI_ICON);
}

/* 开关：顶部开口的圆环 + 贯穿缺口的竖线 */
static void draw_power(int x, int y, int w, int h)
{
	int thick = MAX(1, w / 27);
	int r = w / 4;
	draw_arc(x + h / 2, y + w / 2, r, M_PI + 0.6f, 3 * M_PI - 0.6f, thick, UI_ICON, UI_ICON, UI_ICON);
	draw_thick_line(x + h / 2 - r, y + w / 2, x + h / 2 + r / 2, y + w / 2, w / 12, UI_ICON);
}

/* WiFi 徽标：以 (cx, cy) 为中心画边长为 size 的圆角正方形黑框，
 * 框内为 WiFi 图标；level 为负数时只留空框 */
static void draw_wifi(int cx, int cy, int size, int8_t level)
{
	int thick = MAX(1, size / 24);		/* 边框与弧线线宽 */
	int rad = size * 2 / 5;			/* 图标半径：框内留出边距 */
	int top = rad + thick;			/* 弧顶到图标底部实心点的距离 */
	int dot = cy + (top - thick) / 2;	/* 让图标在正方形框内垂直居中 */
	int k, r;

	fill_round_rect(cx - size / 2 + thick, cy - size / 2 + thick, size - 2 * thick, size - 2 * thick, size / 6, UI_BORDER, UI_BORDER, UI_BORDER);
	fill_round_rect(cx - size / 2 + 2 * thick, cy - size / 2 + 2 * thick, size - 4 * thick, size - 4 * thick, size / 6, UI_SHADOW, UI_SHADOW, UI_SHADOW);

	//fill_round_frame(cx - size / 2 + thick, cy - size / 2 + thick, size - 2 * thick, size - 2 * thick, MAX(2, size / 6), thick, 0, 0, 0);
	if (level < 0)
		return;	// 无信号，只留空框
	fill_circle(cx, dot, thick, UI_ICON, UI_ICON, UI_ICON);
	for (k = 1; k <= 3; k++) {
		float a;
		unsigned col = (level >= k) ? UI_ICON : UI_ICON / 2;
		r = rad * k / 3;
		for (a = 0.6f; a <= M_PI - 0.6f; a += 0.05f) {
			int x = cx + (int) (r * cosf(a));
			int y = dot - (int) (r * sinf(a));
			fill_circle(x, y, thick, col, col, col);
		}
	}
}

/* 电量图标：x 为电池左端，cy 为纵向中心，pct 为电量百分比 */
static void draw_battery(int x, int cy, int w, int h, int pct)
{
	int y = cy - h / 2;
	int rad = h / 5, thick = h / 12;
	int inset = thick + h / 10;
	int nub_w = cfg.ui_w / 6;	// 电极高度
	fill_rect(x, cy - h / 4, nub_w, h / 2, UI_BORDER, UI_BORDER, UI_BORDER);
	fill_round_rect(nub_w + x, y, w - nub_w, h, rad, UI_BORDER, UI_BORDER, UI_BORDER);
	fill_round_rect(nub_w + x + thick, y + thick, w - nub_w - 2 * thick, h - 2 * thick, MAX(1, rad - thick), UI_BG, UI_BG, UI_BG);
	fill_rect(nub_w + x + inset, y + inset, (w - nub_w - 2 * inset) * pct / 100, h - 2 * inset, UI_ICON, UI_ICON, UI_ICON);
}

/* 读取电池电量百分比，失败返回 100 */
static int battery_pct(void)
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

/* 状态栏时间：年 / 月-日 / 时:分 三列竖排，字符逆时针旋转 90°（自底向上书写）。
 * 整体右对齐到 right，返回占用宽度（放不下时为 0） */
static void draw_time(void)
{
	struct tm tm;
	char cols[3][16];
	time_t now = time(NULL);

	if (!localtime_r(&now, &tm))
		return;
	snprintf(cols[0], sizeof(cols[0]), "%04d", tm.tm_year + 1900);
	snprintf(cols[1], sizeof(cols[1]), "%02d-%02d", tm.tm_mon + 1, tm.tm_mday);
	snprintf(cols[2], sizeof(cols[2]), "%02d:%02d", tm.tm_hour, tm.tm_min);

	int cell = cfg.ui_w / 5;	/* 最长一列 5 个字符（月-日、时:分） */
	int y = cfg.scr_w - cfg.ui_w;

	fill_rect(cfg.scr_w - cfg.ui_w, cfg.view_w, cfg.ui_w, cfg.ui_w, UI_SHADOW, UI_SHADOW, UI_SHADOW);
	for (int i = 0; i < 3; i++) {
		int n = (int) strlen(cols[i]);
		/* 各列在高度内居中，再从最下方字符开始向上写 */
		int x0 = -cfg.ui_w + (cfg.ui_w - n * cell) / 2;
		text(x0, y, cols[i], 8 * FBM_FONT_SCALE, UI_ICON, UI_ICON, UI_ICON);
		y += cell + (cfg.ui_w - 3 * cell) / 2;
	}
}

/* 绘制状态图标与按钮 */
void *draw_ui(void *arg)
{
	if (!cfg.ui_w)	// 页面填满屏幕，无空白区域
		return NULL;
	int btn_gap = cfg.ui_w * 2 / 3;	// 按钮间隔
	int btn_h = cfg.ui_w * 3 / 2;	// 按钮高度

	fill_rect(0, cfg.view_w, cfg.ui_h, cfg.ui_w, UI_SHADOW, UI_SHADOW, UI_SHADOW);

	// 仅触屏版需要显示按钮
	for (int i = 0; i < 6; i++) {
		int x = btn_gap / 2 + i * (btn_h + btn_gap);	// 最左边留半个 gap
		draw_button(x, cfg.view_w, cfg.ui_w, btn_h);

		switch (i) {
		case 0: draw_arrow(x, cfg.view_w, cfg.ui_w, btn_h, -1); break;	/* 上一页 */
		case 1: draw_arrow(x, cfg.view_w, cfg.ui_w, btn_h, +1); break;	/* 下一页 */
		case 2: draw_go(x, cfg.view_w, cfg.ui_w, btn_h, -1); break;	/* 确认 */
		case 3: draw_go(x, cfg.view_w, cfg.ui_w, btn_h, +1); break;	/* 返回 */
		case 4: draw_refresh(x, cfg.view_w, cfg.ui_w, btn_h); break;	/* 刷新 */
		case 5: draw_power(x, cfg.view_w, cfg.ui_w, btn_h); break;	/* 开关 */
		}
	}

	while (true) {	/* 靠右依次为 电量、WiFi、时间，时间贴最右边 */
		draw_time();	// 固定在右下角

		int cy = cfg.view_w + cfg.ui_w / 2;
		draw_wifi(cfg.scr_w - 2 * cfg.ui_w + cfg.ui_w / 2, cy, cfg.ui_w, wifi_level());
		draw_battery(cfg.scr_w - 4 * cfg.ui_w, cy, cfg.ui_w * 2, cfg.ui_w * 4 / 5, battery_pct());
		sleep(TIME_PERIOD);
	}
	return NULL;
}
