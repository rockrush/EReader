#CC = clang
CFLAGS = -Wall -O2 -Wunused -Wunused-function

# fontconfig 用于自动发现系统字体；没有它时 fontlist.c 退回扫描字体目录
FC_CFLAGS := $(shell pkg-config --cflags fontconfig 2>/dev/null)
FC_LIBS := $(shell pkg-config --libs fontconfig 2>/dev/null)
ifneq ($(strip $(FC_LIBS)),)
CFLAGS += -DHAVE_FONTCONFIG $(FC_CFLAGS)
endif
SRCS=$(wildcard *.c)
OBJS=${SRCS:.c=.o}

%.o: %.c doc.h
	$(CC) -c $(CFLAGS) $(shell pkg-config --cflags freetype2) $<

ereader: poppler.cc $(OBJS)
	$(CXX) $(CFLAGS) -o $@ $^ $(FC_LIBS) $(shell pkg-config --cflags --libs poppler-cpp) $(shell pkg-config --libs libfyaml freetype2)

.PHONY: clean count
clean:
	-@rm -f *.o ereader

count:
	@wc *.c *.h
