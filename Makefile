#CC = clang
CFLAGS = -Wall -O2 -Wunused -Wunused-function
TARGETS = ereader

all: $(TARGETS)

%.o: %.c doc.h
	$(CC) -c $(CFLAGS) $<

# PDF support using poppler
ereader: ereader.o poppler.c draw.o
	$(CXX) $(CFLAGS) -o $@ $^ $(shell pkg-config --cflags --libs poppler-cpp) $(shell pkg-config --libs libfyaml)

.PHONY: clean count
clean:
	-@rm -f *.o $(TARGETS)

count:
	@wc doc.h draw.c draw.h ereader.c poppler.c
