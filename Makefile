CC = gcc
CFLAGS = -c -Wall -g -shared -fPIC
LDFLAGS = -shared -fPIC

SOURCES = xfconf-gsettings-backend.c
OBJECTS = $(SOURCES:.c=.o)
LIBS = libxfconf-0 glib-2.0 gio-2.0 gio-unix-2.0
GSETTINGS_SO = libxfconfsettings.so

CFLAGS += `pkg-config --cflags ${LIBS}`
LDFLAGS += `pkg-config --libs ${LIBS}`

all: $(SOURCES) $(GSETTINGS_SO)
	gio-querymodules .

$(GSETTINGS_SO): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

.c.o:
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm $(OBJECTS) $(GSETTINGS_SO)
	rm giomodule.cache
