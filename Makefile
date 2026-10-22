CFLAGS = -Wall -O2 -Wunused -Wunused-function
LDFLAGS = $(shell pkg-config --libs libfyaml freetype2 fontconfig poppler-cpp)
SRCS=$(wildcard *.c)
OBJS=${SRCS:.c=.o} poppler.o

%.o: %.c doc.h
	$(CC) -c $(CFLAGS) $(shell pkg-config --cflags freetype2 fontconfig) $<

ereader: $(OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS)

.PHONY: clean count
clean:
	-@rm -f *.o ereader

count:
	@wc *.c *.h
