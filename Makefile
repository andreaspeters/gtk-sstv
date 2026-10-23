CC = gcc

CFLAGS    = -Wall -Wextra -std=gnu99 -pedantic -g
GTKCFLAGS = `pkg-config --cflags gtk4`
GTKLIBS   = `pkg-config --libs gtk4`

OFLAGS = -O3

OBJECTS = common.o modespec.o gui.o video.o vis.o sync.o pcm.o fsk.o slowrx.o encoder.o compose.o txaudio.o txdialog.o adifudp.o

all: sstv

sstv: $(OBJECTS)
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

adifudp.o: adifudp.c adifudp.h
	$(CC) $(CFLAGS) $(GTKCFLAGS) $(OFLAGS) -c -o $@ $<

TEST_BUILD_DIR ?= /data/build-nix
.PHONY: test-adifudp
test-adifudp:
	@mkdir -p "$(TEST_BUILD_DIR)"
	$(CC) $(CFLAGS) $(GTKCFLAGS) -I. -o "$(TEST_BUILD_DIR)/test_adifudp" \
	  test_adifudp.c adifudp.c $(GTKLIBS) -lm
	"$(TEST_BUILD_DIR)/test_adifudp"

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
	$(CC) -Wall -Wextra -std=gnu99 -pedantic $$(pkg-config --cflags gtk4) -I. \
	  -o "$$tmp" test_compose.c compose.c $$(pkg-config --libs gtk4) -lm && "$$tmp"; \
	result=$$?; rm -f "$$tmp"; exit $$result

.PHONY: test-reply-fallback
test-reply-fallback:
	@mkdir -p "$(HOME)/.hermes/cache/scratch"; \
	tmpdir=$$(mktemp -d "$(HOME)/.hermes/cache/scratch/slowrx-reply-fallback.XXXXXX"); \
	trap 'rm -rf "$$tmpdir"' EXIT; \
	$(CC) $(CFLAGS) $(GTKCFLAGS) $$(pkg-config --cflags alsa fftw3) -I. \
	  -ffunction-sections -fdata-sections -Dmain=slowrx_application_main \
	  -c slowrx.c -o "$$tmpdir/slowrx.o" && \
	$(CC) $(CFLAGS) $(GTKCFLAGS) $$(pkg-config --cflags alsa fftw3) -I. \
	  -ffunction-sections -fdata-sections -Wl,--gc-sections \
	  -o "$$tmpdir/test" test_reply_fallback.c common.c modespec.c gui.c video.c \
	  vis.c sync.c pcm.c fsk.c encoder.c compose.c txaudio.c adifudp.c \
	  "$$tmpdir/slowrx.o" $(GTKLIBS) -lfftw3 -lasound -lm -lpthread && \
	xvfb-run -a "$$tmpdir/test"

.PHONY: test-txaudio
test-txaudio:
	@mkdir -p "$(HOME)/.hermes/cache/scratch"; \
	tmp=$$(mktemp "$(HOME)/.hermes/cache/scratch/slowrx-txaudio-test.XXXXXX"); \
	$(CC) -Wall -Wextra -std=gnu99 -pedantic $$(pkg-config --cflags alsa glib-2.0) -I. \
	  -o "$$tmp" test_txaudio.c txaudio.c $$(pkg-config --libs alsa glib-2.0) && "$$tmp"; \
	result=$$?; rm -f "$$tmp"; exit $$result

.PHONY: test-vis
test-vis:
	@mkdir -p "$(HOME)/.hermes/cache/scratch"; \
	tmp=$$(mktemp "$(HOME)/.hermes/cache/scratch/slowrx-vis-test.XXXXXX"); \
	$(CC) -Wall -Wextra -std=gnu99 -pedantic $$(pkg-config --cflags gtk4 alsa fftw3) -I. \
	  -ffunction-sections -fdata-sections -Wl,--wrap=snd_pcm_readi \
	  -Wl,--gc-sections -o "$$tmp" test_vis.c vis.c pcm.c modespec.c common.c \
	  $$(pkg-config --libs gtk4 alsa fftw3) -lm -lpthread && "$$tmp"; \
	result=$$?; rm -f "$$tmp"; exit $$result

.PHONY: test-sync-bounds
test-sync-bounds:
	@mkdir -p "$(HOME)/.hermes/cache/scratch"; \
	tmp=$$(mktemp "$(HOME)/.hermes/cache/scratch/slowrx-sync-bounds-test.XXXXXX"); \
	$(CC) -Wall -Wextra -std=gnu99 -pedantic $(GTKCFLAGS) \
	  -ffunction-sections -fdata-sections -Wl,--gc-sections -I. \
	  -o "$$tmp" test_sync_bounds.c sync.c $(GTKLIBS) -lfftw3 -lasound -lm -lpthread && "$$tmp"; \
	result=$$?; rm -f "$$tmp"; exit $$result

.PHONY: test-frequency-guides
test-frequency-guides:
	@mkdir -p "$(HOME)/.hermes/cache/scratch"; \
	tmp=$$(mktemp "$(HOME)/.hermes/cache/sstv-frequency-guides-test.XXXXXX"); \
	$(CC) -Wall -Wextra -std=gnu99 -pedantic $(GTKCFLAGS) \
	  `pkg-config --cflags alsa fftw3` -ffunction-sections -fdata-sections \
	  -Wl,--gc-sections -I. -o "$$tmp" test_frequency_guides.c common.c modespec.c \
	  $(GTKLIBS) -lasound -lfftw3 -lm -lpthread && xvfb-run -a "$$tmp"; \
	result=$$?; rm -f "$$tmp"; exit $$result

.PHONY: test-recent-pictures
test-recent-pictures:
	@mkdir -p "$(HOME)/.hermes/cache/scratch"; \
	tmp=$$(mktemp "$(HOME)/.hermes/cache/scratch/sstv-recent-pictures-test.XXXXXX"); \
	$(CC) -Wall -Wextra -std=gnu99 -pedantic $(GTKCFLAGS) -I. \
	  -ffunction-sections -fdata-sections -Wl,--gc-sections \
	  -o "$$tmp" test_recent_pictures.c modespec.c $(GTKLIBS) \
	  -lfftw3 -lasound -lm -lpthread && xvfb-run -a "$$tmp"; \
	result=$$?; rm -f "$$tmp"; exit $$result

clean:
	rm -f sstv $(OBJECTS)
