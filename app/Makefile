PROG1     = $(shell jq -r '.acapPackageConf.setup.appName' manifest.json)
OBJS1     = src/main.o src/config.o src/mqtt.o src/dedupe.o src/scene.o src/events.o
PROGS     = $(PROG1)
DEBUG_DIR = debug

MOSQUITTO_PREFIX ?= /opt/mosquitto

PKGS = glib-2.0 gio-2.0 axevent axparameter mdb jansson

CFLAGS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags $(PKGS))
CFLAGS += -I$(MOSQUITTO_PREFIX)/include
LDLIBS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs $(PKGS))
LDLIBS += $(MOSQUITTO_PREFIX)/lib/libmosquitto.a -lpthread -lm

CFLAGS += -Wall \
          -Wextra \
          -Wformat=2 \
          -Wpointer-arith \
          -Wstrict-prototypes \
          -Wmissing-prototypes \
          -Werror

all: $(PROGS)

$(PROG1): $(OBJS1)
	install -d $(DEBUG_DIR)
	$(CC) $^ $(CFLAGS) $(LDFLAGS) $(LDLIBS) -o $(DEBUG_DIR)/$@
	cp $(DEBUG_DIR)/$@ .
	$(STRIP) $@

clean:
	rm -rf $(PROGS) src/*.o *.eap* *_LICENSE.txt package.conf* param.conf tmp* $(DEBUG_DIR)
