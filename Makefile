CC = gcc

CFLAGS    = -Wall -Wextra -std=gnu99 -pedantic -g -DGDK_VERSION_MIN_REQUIRED=GDK_VERSION_3_4
GTKCFLAGS = `pkg-config --cflags gtk+-3.0`
GTKLIBS   = `pkg-config --libs gtk+-3.0`

OFLAGS = -O3

OBJECTS = common.o modespec.o gui.o video.o vis.o sync.o pcm.o fsk.o slowrx.o encoder.o compose.o txaudio.o txdialog.o

all: slowrx

slowrx: $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $(OBJECTS) $(GTKLIBS) -lfftw3 -lgthread-2.0 -lasound -lm -lpthread

%.o: %.c common.h
	$(CC) $(CFLAGS) $(GTKCFLAGS) $(OFLAGS) -c -o $@ $<

encoder.o: encoder.c encoder.h
	$(CC) $(CFLAGS) $(GTKCFLAGS) $(OFLAGS) -c -o $@ $<

compose.o: compose.c compose.h
	$(CC) $(CFLAGS) $(GTKCFLAGS) $(OFLAGS) -c -o $@ $<

txaudio.o: txaudio.c txaudio.h
	$(CC) $(CFLAGS) $(GTKCFLAGS) $(OFLAGS) -c -o $@ $<

txdialog.o: txdialog.c txdialog.h compose.h encoder.h txaudio.h
	$(CC) $(CFLAGS) $(GTKCFLAGS) $(OFLAGS) -c -o $@ $<

.PHONY: test-encoder
test-encoder:
	@mkdir -p "$(HOME)/.hermes/cache/scratch"; \
	tmp=$$(mktemp "$(HOME)/.hermes/cache/scratch/slowrx-encoder-test.XXXXXX"); \
	$(CC) -Wall -Wextra -std=gnu99 -pedantic $$(pkg-config --cflags gdk-pixbuf-2.0) -I. \
	  -o "$$tmp" test_encoder.c encoder.c $$(pkg-config --libs gdk-pixbuf-2.0) -lm && "$$tmp"; \
	result=$$?; rm -f "$$tmp"; exit $$result

.PHONY: test-compose
test-compose:
	@mkdir -p "$(HOME)/.hermes/cache/scratch"; \
	tmp=$$(mktemp "$(HOME)/.hermes/cache/scratch/slowrx-compose-test.XXXXXX"); \
	$(CC) -Wall -Wextra -std=gnu99 -pedantic $$(pkg-config --cflags gtk+-3.0) -I. \
	  -o "$$tmp" test_compose.c compose.c $$(pkg-config --libs gtk+-3.0) -lm && "$$tmp"; \
	result=$$?; rm -f "$$tmp"; exit $$result

.PHONY: test-txaudio
test-txaudio:
	@mkdir -p "$(HOME)/.hermes/cache/scratch"; \
	tmp=$$(mktemp "$(HOME)/.hermes/cache/scratch/slowrx-txaudio-test.XXXXXX"); \
	$(CC) -Wall -Wextra -std=gnu99 -pedantic $$(pkg-config --cflags alsa glib-2.0) -I. \
	  -o "$$tmp" test_txaudio.c txaudio.c $$(pkg-config --libs alsa glib-2.0) && "$$tmp"; \
	result=$$?; rm -f "$$tmp"; exit $$result

clean:
	rm -f slowrx $(OBJECTS)
