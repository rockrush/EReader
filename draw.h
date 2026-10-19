/* fb_mode() interpretation */
#define FBM_BPP(m)	(((m) >> 16) & 0x0f)	/* bytes per pixel (4 bits) */
#define FBM_CLR(m)	((m) & 0x0fff)		/* bits per color (12 bits) */
#define FBM_ORD(m)	(((m) >> 20) & 0x07)	/* color order (3 bits) */

int fb_init(char *dev);
void fb_free(void);
unsigned fb_mode(void);
void *fb_mem(int r);
int fb_rows(void);
int fb_cols(void);
void fb_cmap(void);
unsigned fb_val(int r, int g, int b);
void fb_get(char *d, unsigned *r, unsigned *g, unsigned *b);

void term_setup(struct termios *term);
void term_cleanup(struct termios *term);
