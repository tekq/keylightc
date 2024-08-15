all: 
	$(CC) -O2 -Werror -Wall -pedantic $(CFLAGS) -o keylightc keylightc.c

install:
	install -d $(DESTDIR)/usr/bin/
	install -m 755 keylightc $(DESTDIR)/usr/bin/
	install -d $(DESTDIR)/usr/lib/systemd/system/
	install -m 644 keylightc.service $(DESTDIR)/usr/lib/systemd/system/
