CROSS_COMPILE ?= arm-linux-
CC := $(CROSS_COMPILE)gcc

CFLAGS := -O2 -ggdb -Wall -Wextra -Wno-unused-parameter -Wmissing-prototypes -ansi -std=c99
LDFLAGS := -Wl,--no-undefined -Wl,--as-needed

override CFLAGS += -D_GNU_SOURCE -DGST_DISABLE_DEPRECATED

GST_CFLAGS := $(shell pkg-config --cflags gstreamer-0.10 gstreamer-base-0.10)
GST_LIBS := $(shell pkg-config --libs gstreamer-0.10 gstreamer-base-0.10)

DRM_CFLAGS := $(shell pkg-config --cflags libdrm libkms)
DRM_LIBS := $(shell pkg-config --libs libdrm libkms)

all:

version := $(shell ./get-version)
prefix := /usr


D = $(DESTDIR)

# plugin

libgstdrmsink.so: drmsink.o log.o
libgstdrmsink.so: override CFLAGS += $(GST_CFLAGS) $(DRM_CFLAGS) -fPIC -D VERSION='"$(version)"'
libgstdrmsink.so: override LIBS += $(GST_LIBS) $(DRM_LIBS)

targets += libgstdrmsink.so

all: $(targets)

# pretty print
ifndef V
QUIET_CC    = @echo '   CC         '$@;
QUIET_LINK  = @echo '   LINK       '$@;
QUIET_CLEAN = @echo '   CLEAN      '$@;
endif

install: $(targets)
	install -m 755 -D libgstdrmsink.so $(D)/$(prefix)/lib/gstreamer-0.10/libgstdrmsink.so

%.o:: %.c
	$(QUIET_CC)$(CC) $(CFLAGS) -MMD -o $@ -c $<

%.so::
	$(QUIET_LINK)$(CC) $(LDFLAGS) -shared -o $@ $^ $(LIBS)

clean:
	$(QUIET_CLEAN)$(RM) -v $(targets) *.o *.d

dist: base := gst-drmsink-$(version)
dist:
	git archive --format=tar --prefix=$(base)/ HEAD > /tmp/$(base).tar
	mkdir -p $(base)
	echo $(version) > $(base)/.version
	chmod 664 $(base)/.version
	tar --append -f /tmp/$(base).tar --owner root --group root $(base)/.version
	rm -r $(base)
	gzip /tmp/$(base).tar

-include *.d
