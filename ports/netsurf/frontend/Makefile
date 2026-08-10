#
# NetSurf frontend for EmbLinkOS.
#
# Symlinked into the NetSurf source tree as frontends/emblink by
# ports/netsurf/build.sh, so the code stays in OUR repo and under our history
# while NetSurf's tree stays pristine -- the port has no patches and this is
# part of how it keeps that.
#

# -std=c99 defines __STRICT_ANSI__, which hides everything newlib considers an
# extension -- including M_PI, which the core uses to draw a pie chart. The
# feature macros put the misc set back; every other frontend does the same.
CFLAGS += -std=c99 -D_DEFAULT_SOURCE -D_BSD_SOURCE -D_XOPEN_SOURCE=700 \
	  -D_POSIX_C_SOURCE=200809L \
	  -Demblink -Dnsemblink \
	  -DEMBLINK_RESPATH=\"$(NETSURF_EMBLINK_RESOURCES)\" \
	  -DFONT_NO_BACKEND -I$(EMBLINK_ROOT)/ui/backend

# The OS has no -lm separate from libc, and no POSIX feature macros to enable:
# newlib exposes what it has unconditionally.
LDFLAGS +=

EMBLINK_RESOURCES_DIR := $(FRONTEND_RESOURCES_DIR)
MESSAGES_FILTER=emblink
MESSAGES_TARGET=$(EMBLINK_RESOURCES_DIR)

# One file per concern, which is the house rule and also the only way the
# frontend stays readable: each of these implements exactly one of NetSurf's
# tables, and the table is the contract.
S_FRONTEND := main.c misc.c window.c fetch.c layout.c bitmap.c plot.c text.c

SOURCES = $(S_COMMON) $(S_IMAGE) $(S_BROWSER) $(S_FRONTEND)
EXETARGET := nsemblink

install-emblink:
package-emblink:
