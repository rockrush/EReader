#ifndef FONTLIST_H
#define FONTLIST_H
int font_line_height(int px);
int font_advance(unsigned int cp, int px);
unsigned int font_utf8(const char **s);
int text_n(int x, int y, const char *s, size_t len, int px, unsigned r, unsigned g, unsigned b);
int text(int x, int y, const char *s, int px, unsigned r, unsigned g, unsigned b);
#endif
