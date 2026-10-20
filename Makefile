#CC = clang
CFLAGS = -Wall -O2 -Wunused -Wunused-function $(shell pkg-config --cflags freetype2)
LDLIBS = $(shell pkg-config --libs freetype2)
TARGETS = ereader

all: $(TARGETS)

%.o: %.c doc.h
	$(CC) -c $(CFLAGS) $<

# PDF support using poppler
ereader: ereader.o poppler.c draw.o filemgr.o text.o
	$(CXX) $(CFLAGS) -o $@ $^ $(shell pkg-config --cflags --libs poppler-cpp) $(shell pkg-config --libs libfyaml) $(LDLIBS)

.PHONY: clean count
clean:
	-@rm -f *.o $(TARGETS)

count:
	@wc *.c *.h
