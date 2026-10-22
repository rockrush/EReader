#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ereader.h"
#include "draw.h"
#include "doc.h"

#define MARGIN		1
#define TRIM_MARGIN	8	// 去白边后内容四周保留的边距（像素）

int zoom = 280;
struct doc *doc;
char *pbuf;		// current page
int prows, pcols;	// current page dimensions
int pcol;		// page position
int scol;		// screen position
extern int bpp;
extern int cur_page;

void fb_set(char *d, unsigned r, unsigned g, unsigned b);

void draw(void)
{
	int i, j;
	char gray[4];
	char *rbuf = malloc(cfg.scr_w * bpp);
	fb_set(gray, UI_SHADOW, UI_SHADOW, UI_SHADOW);
	for (i = 0; i < cfg.scr_h - cfg.ui_w; i++) {
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
void fitpage(int p)
{
	int w, h;
	if (doc_size(doc, p, &w, &h))
		return;
	/* 固定 270 度：宽高互换（旋转后页面横向） */
	int t = w;
	w = h;
	h = t;

	if (w <= 0 || h <= 0)
		return;
	zoom = cfg.scr_w * 100 / w;
	zoom = MIN(1000, MAX(1, zoom));
}

/* 检测页面内容（非纯白像素）的包围盒，返回是否找到内容 */
int content_bbox(char *buf, int rows, int cols, int bpp, int *x0, int *y0, int *x1, int *y1)
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
char *crop_page(char *buf, int *rows, int *cols, int bpp, int x0, int y0, int x1, int y1, int margin)
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
void scale_bilinear(char *src, int sw, int sh, char *dst, int dw, int dh, int bpp)
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
int renderpage(int p)
{
	char *buf;
	int cx0, cy0, cx1, cy1;
	int tw, th;
	char *dst;

	fitpage(p);
	buf = doc_draw(doc, p, zoom, bpp, &prows, &pcols);
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

int loadpage(int p)
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

int reload(char *path)
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

int show_pdf(char *path)
{
	int c, retval = EXIT_FAILURE;
	if (!path || !strlen(path)) {	// 此时显示文件管理器
		fprintf(stderr, "no PDF file specified\n");
		goto fail_file;
	}
	doc = doc_open(path);
	if (!doc || !doc_pages(doc)) {
		fprintf(stderr, "cannot open <%s>\n", path);
		goto fail_file;
	}

	if ((cur_page < 1) || (cur_page > doc_pages(doc)))
		cur_page = 1;
	loadpage(cur_page);
	scol = - cfg.view_h / 2;
	draw();

	while ((c = readkey()) != -1) {
		printf("show_pdf: %d, %d\n", prows, pcols);
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
