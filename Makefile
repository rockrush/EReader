#CC = clang
CFLAGS = -Wall -O2 -Wunused -Wunused-function
SRCS=$(wildcard *.c)
OBJS=${SRCS:.c=.o}

%.o: %.c doc.h
	$(CC) -c $(CFLAGS) $(shell pkg-config --cflags freetype2) $<

ereader: poppler.cc $(OBJS)
	$(CXX) $(CFLAGS) -o $@ $^ $(shell pkg-config --cflags --libs poppler-cpp) $(shell pkg-config --libs libfyaml freetype2)

.PHONY: clean count
clean:
	-@rm -f *.o ereader

count:
	@wc *.c *.h
