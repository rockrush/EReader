#include <stdlib.h>
#include <string.h>
#include <poppler/cpp/poppler-document.h>
#include <poppler/cpp/poppler-image.h>
#include <poppler/cpp/poppler-page.h>
#include <poppler/cpp/poppler-page-renderer.h>

extern "C" {
#include "ereader.h"
#include "draw.h"
#include "doc.h"
void fb_set(char *d, unsigned r, unsigned g, unsigned b);

int doc_size(struct doc *doc, int p, int *width, int *height)
{
	if (!doc || !doc->hdl)
		return 1;
	poppler::document *pdf = (poppler::document *)doc->hdl;
	poppler::page *page = pdf->create_page(p - 1);
	poppler::rectf r;
	if (!page)
		return 1;
	r = page->page_rect();
	*width = (int) (r.width() + 0.5);
	*height = (int) (r.height() + 0.5);
	delete page;
	return 0;
}

void *doc_draw(struct doc *doc, int p, int bpp, int *rows, int *cols)
{
	int x, y;
	int h, w;
	char *pbuf;
	unsigned char *dat;
	poppler::document *pdf = (poppler::document *)doc->hdl;
	poppler::page *page = pdf->create_page(p - 1);
	poppler::page_renderer pr;
	poppler::rectf r;

	if (!page)
		return NULL;
	r = page->page_rect();

	int pw = (int)(r.height() + 0.5);
	if (pw <= 0)

	if (!doc || !doc->hdl)
		return NULL;
	int zoom = cfg.scr_w * 100 / pw;
	zoom = MIN(1000, MAX(1, zoom));

	pr.set_render_hint(poppler::page_renderer::antialiasing, true);
	pr.set_render_hint(poppler::page_renderer::text_antialiasing, true);
	poppler::image img = pr.render_page(page, (float) 72 * zoom / 100,
		(float) 72 * zoom / 100, -1, -1, -1, -1, poppler::rotate_270);
	h = img.height();
	w = img.width();
	dat = (unsigned char *) img.data();
	if (!(pbuf = (char *) malloc(h * w * bpp))) {
		delete page;
		return NULL;
	}
	for (y = 0; y < h; y++) {
		unsigned char *s = dat + img.bytes_per_row() * y;
		char *d = pbuf + (y * w) * bpp;
		for (x = 0; x < w; x++)
			fb_set(d + x * bpp, s[x * 4 + 2], s[x * 4 + 1], s[x * 4]);
	}
	*rows = h;
	*cols = w;
	delete page;
	return pbuf;
}

int doc_pages(struct doc *doc)
{
	if (!doc || !doc->hdl)
		return 1;
	poppler::document *pdf = (poppler::document *)doc->hdl;
	return pdf->pages();
}

void doc_close(struct doc *doc)
{
	if (!doc || !doc->hdl)
		return;
	delete (poppler::document *)doc->hdl;
	free(doc);
}

struct doc *pdf_open(char *path)
{
	struct doc *doc = (struct doc *)malloc(sizeof(*doc));
	if (doc == NULL)
		return NULL;
	poppler::document *pdf = poppler::document::load_from_file(path);
	if (!pdf) {
		free(doc);
		return NULL;
	}
	doc->hdl = (void *)pdf;
	doc->pages = doc_pages;
	doc->size = doc_size;
	doc->draw = doc_draw;
	doc->close = doc_close;
	return doc;
}
}	// extern "C"
