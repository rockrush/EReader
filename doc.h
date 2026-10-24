struct doc {
	void *hdl;
	int (*pages)(struct doc *doc);
	int (*size)(struct doc *doc, int page, int *width, int *height);
	void *(*draw)(struct doc *doc, int page, int bpp, int *rows, int *cols);
	void (*close)(struct doc *doc);
};

struct doc *pdf_open(char *path);
int doc_pages(struct doc *doc);
int doc_size(struct doc *doc, int page, int *width, int *height);
void *doc_draw(struct doc *doc, int page, int bpp, int *rows, int *cols);
void doc_close(struct doc *doc);
