CFLAGS = -Wall -O2 -Wunused -Wunused-function $(shell pkg-config --cflags fontconfig)
LDFLAGS = $(shell pkg-config --libs libfyaml freetype2 fontconfig poppler-cpp)
SRCS=$(wildcard *.c)
OBJS=${SRCS:.c=.o}

%.o: %.c doc.h
	$(CC) -c $(CFLAGS) $(shell pkg-config --cflags freetype2) $<

ereader: poppler.cc $(OBJS)
	$(CXX) $(CFLAGS) -o $@ $^ $(FC_LIBS) $(shell pkg-config --cflags poppler-cpp) $(LDFLAGS)

.PHONY: clean count
clean:
	-@rm -f *.o ereader

count:
	@wc *.c *.h
