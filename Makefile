CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LDFLAGS = -lcurl

PREFIX  = /usr
BINDIR  = $(PREFIX)/bin
CONFDIR = /etc/vibeinterpreter

TARGET  = vibeinterpreter

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): vibeinterpreter.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	ln -sf $(BINDIR)/$(TARGET) $(DESTDIR)/bin/$(TARGET)
	install -d $(DESTDIR)$(CONFDIR)
	@if [ ! -f $(DESTDIR)$(CONFDIR)/api.secret ]; then \
		touch $(DESTDIR)$(CONFDIR)/api.secret; \
		chmod 600 $(DESTDIR)$(CONFDIR)/api.secret; \
		echo "=> Place your Anthropic API key in $(CONFDIR)/api.secret"; \
	fi

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f $(DESTDIR)/bin/$(TARGET)

clean:
	rm -f $(TARGET)
