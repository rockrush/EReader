#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <sys/mman.h>
#include <linux/kd.h>

#include "ereader.h"
#include "draw.h"

#define NLEVELS		(1 << 8)

static struct fb_var_screeninfo vinfo;	/* linux-specific FB structure */
static struct fb_fix_screeninfo finfo;	/* linux-specific FB structure */
static int fd;				/* FB device file descriptor */
static void *fb;			/* mmap()ed FB memory */
extern int bpp;				/* bytes per pixel */
static int nr, ng, nb;			/* color levels */
static int rl, rr, gl, gr, bl, br;	/* shifts per color */
static int xoff, yoff;			/* drawing region */
static int tty_in = -1;			/* tty for reading keys */
static struct termios tty_orig;		/* tty state before graphics mode */
static bool tty_orig_valid;

static int fb_len(void)
{
	return finfo.line_length * vinfo.yres_virtual;
}

static void fb_cmap_save(int save)
{
	static unsigned short red[NLEVELS], green[NLEVELS], blue[NLEVELS];
	struct fb_cmap cmap;
	if (finfo.visual == FB_VISUAL_TRUECOLOR)
		return;
	cmap.start = 0;
	cmap.len = MAX(nr, MAX(ng, nb));
	cmap.red = red;
	cmap.green = green;
	cmap.blue = blue;
	cmap.transp = NULL;
	ioctl(fd, save ? FBIOGETCMAP : FBIOPUTCMAP, &cmap);
}

void fb_cmap(void)
{
	unsigned short red[NLEVELS], green[NLEVELS], blue[NLEVELS];
	struct fb_cmap cmap;
	int i;
	if (finfo.visual == FB_VISUAL_TRUECOLOR)
		return;

	for (i = 0; i < nr; i++)
		red[i] = (65535 / (nr - 1)) * i;
	for (i = 0; i < ng; i++)
		green[i] = (65535 / (ng - 1)) * i;
	for (i = 0; i < nb; i++)
		blue[i] = (65535 / (nb - 1)) * i;

	cmap.start = 0;
	cmap.len = MAX(nr, MAX(ng, nb));
	cmap.red = red;
	cmap.green = green;
	cmap.blue = blue;
	cmap.transp = NULL;

	ioctl(fd, FBIOPUTCMAP, &cmap);
}

static void init_colors(void)
{
	nr = 1 << vinfo.red.length;
	ng = 1 << vinfo.blue.length;
	nb = 1 << vinfo.green.length;
	rr = 8 - vinfo.red.length;
	rl = vinfo.red.offset;
	gr = 8 - vinfo.green.length;
	gl = vinfo.green.offset;
	br = 8 - vinfo.blue.length;
	bl = vinfo.blue.offset;
}

int fb_init(char *dev)
{
	char *path = dev;
	fd = open(path, O_RDWR);
	if (fd < 0)
		goto failed;
	if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0)
		goto failed;
	if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0)
		goto failed;
	fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
	bpp = (vinfo.bits_per_pixel + 7) >> 3;
	fb = mmap(NULL, fb_len(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED)
		goto failed;
	init_colors();
	fb_cmap_save(1);
	fb_cmap();

	cfg.scr_h = vinfo.yres;
	cfg.scr_w = vinfo.xres;
	cfg.ui_w = cfg.scr_h - cfg.scr_w * 210 / 297;	// A4 幅面剩余部分显示UI
	if (cfg.ui_w < 32)
		cfg.ui_w = 0;
	cfg.view_w = cfg.scr_h - cfg.ui_w;
	return 0;
failed:
	perror("fb_init()");
	close(fd);
	return 1;
}

void fb_free(void)
{
	fb_cmap_save(0);
	munmap(fb, fb_len());
	close(fd);
}

void *fb_mem(int r)
{
	return fb + (r + vinfo.yoffset + yoff) * finfo.line_length + (vinfo.xoffset + xoff) * bpp;
}

unsigned fb_val(int r, int g, int b)
{
	return ((r >> rr) << rl) | ((g >> gr) << gl) | ((b >> br) << bl);
}

/* fb_set() 的逆运算：把帧缓冲字节序的一个像素解码回 8 位 RGB */
void fb_get(char *d, unsigned *r, unsigned *g, unsigned *b)
{
	unsigned c = 0;
	int i;
	for (i = 0; i < bpp; i++)
		c |= (unsigned)(unsigned char)d[i] << (i << 3);
	*r = ((c >> rl) & ((1u << vinfo.red.length) - 1)) << rr;
	*g = ((c >> gl) & ((1u << vinfo.green.length) - 1)) << gr;
	*b = ((c >> bl) & ((1u << vinfo.blue.length) - 1)) << br;
}

// TODO: 用按钮读取函数替换键盘
/* 读键优先用 stdin：从本机控制台或串口/ssh 登录都能用；stdin 被重定向时退回 /dev/tty0 */
static int tty_in_fd(void)
{
	if (tty_in >= 0)
		return tty_in;
	tty_in = isatty(STDIN_FILENO) ? STDIN_FILENO : open("/dev/tty0", O_RDWR | O_NOCTTY);
	return tty_in;
}

/* 关闭行缓冲和回显：GRAPHICS 模式下按键不再有回显，若仍处于 ICANON，
 * read() 要等回车才返回，单键就读不到；保留 ISIG 使 Ctrl+C 依然有效 */
static int key_raw(bool raw)
{
	struct termios t;
	int fd = tty_in_fd();

	if (fd < 0)
		return -1;
	if (!tty_orig_valid) {
		if (!raw)
			return 0;
		if (tcgetattr(fd, &tty_orig) < 0)
			return -1;
		tty_orig_valid = true;
	}
	t = tty_orig;
	if (raw) {
		t.c_lflag &= ~(ICANON | ECHO);
		t.c_cc[VMIN] = 1;
		t.c_cc[VTIME] = 0;
	} else {
		tty_orig_valid = false;
	}
	return tcsetattr(fd, TCSANOW, &t);
}

/* GRAPHICS 模式下 fbcon 将停止绘制光标和文本，但键盘仍按 K_XLATE 送进 tty，
 * 所以 Ctrl+C 等信号依然有效；读键还必须关掉行缓冲，否则 read() 要等回车 */
void graph_mode(bool state) {
	int tty_fd = open("/dev/tty0", O_RDWR);

	/* 终端设置独立于 /dev/tty0：没有切换 VT 的权限时 readkey() 也要能读到单键 */
	if (state && key_raw(true) < 0)
		perror("tcsetattr");

	if (tty_fd < 0)
		perror("open /dev/tty0");
	else {
		if (ioctl(tty_fd, KDSKBMODE, K_XLATE) < 0)
			perror("ioctl KDSKBMODE K_XLATE");
		if (ioctl(tty_fd, KDSETMODE, state ? KD_GRAPHICS : KD_TEXT) < 0)
			perror(state ? "ioctl KDSETMODE KD_GRAPHICS" : "ioctl KDSETMODE KD_TEXT");
		close(tty_fd);
	}

	if (!state && key_raw(false) < 0)
		perror("tcsetattr");
}

int readkey(void)
{
	unsigned char b;
	int fd = tty_in_fd();
	ssize_t n;

	if (fd < 0)
		return -1;
	do
		n = read(fd, &b, 1);
	while (n < 0 && errno == EINTR);
	return (n <= 0) ? -1 : b;
}
