CC = gcc
CFLAGS = -c -Wall -g3 -Og -shared -fPIC
LDFLAGS = -fPIC -lm

GSETTINGS_SO_SOURCES = xfconf-gsettings-backend.c
GSETTINGS_SO_OBJECTS = $(GSETTINGS_SO_SOURCES:.c=.o)
GSETTINGS_SO_LIBS = libxfconf-0 glib-2.0 gio-2.0 gio-unix-2.0
GSETTINGS_SO_CFLAGS = `pkg-config --cflags ${GSETTINGS_SO_LIBS}`
GSETTINGS_SO_LDFLAGS = -shared `pkg-config --libs ${GSETTINGS_SO_LIBS}`
GSETTINGS_SO = libxfconfsettings.so

MIGRATE_SOURCES = migrate-settings.c
MIGRATE_OBJECTS = $(MIGRATE_SOURCES:.c=.o)
MIGRATE_LIBS = glib-2.0 gio-2.0 gio-unix-2.0
MIGRATE_CFLAGS = `pkg-config --cflags ${MIGRATE_LIBS}` -DGIO_MODULE_DIR=\"$(GIO_MODULE_DIR)\"
MIGRATE_LDFLAGS = `pkg-config --libs ${MIGRATE_LIBS}`
MIGRATE = migrate-settings
GIO_MODULE_DIR = `pkg-config --variable giomoduledir gio-2.0`

all: $(GSETTINGS_SO) $(MIGRATE)
	gio-querymodules .

$(GSETTINGS_SO): $(GSETTINGS_SO_OBJECTS)
	$(CC) $(GSETTINGS_SO_OBJECTS) -o $@ $(LDFLAGS) $(GSETTINGS_SO_LDFLAGS)

$(GSETTINGS_SO_OBJECTS): $(GSETTINGS_SO_SOURCES)
	$(CC) $(CFLAGS) $(GSETTINGS_SO_CFLAGS) $< -o $@

$(MIGRATE): $(MIGRATE_OBJECTS)
	$(CC) $(MIGRATE_OBJECTS) -o $@ $(LDFLAGS) $(MIGRATE_LDFLAGS)

$(MIGRATE_OBJECTS): $(MIGRATE_SOURCES)
	$(CC) $(CFLAGS) $(MIGRATE_CFLAGS) $< -o $@

clean:
	rm -f $(GSETTINGS_SO_OBJECTS) $(GSETTINGS_SO) $(MIGRATE_OBJECTS) $(MIGRATE)
	rm -f giomodule.cache
