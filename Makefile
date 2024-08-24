all: 
	$(CC) -O2 -std=c2x -Werror -Wall -pedantic $(CFLAGS) -o keylightc keylightc.c

install:
	install -m 755 -D -t $(DESTDIR)/usr/bin/ keylightc
	install -m 644 -D -t $(DESTDIR)/usr/lib/systemd/system/ keylightc.service
	install -m 644 -D -t $(DESTDIR)/usr/lib/udev/rules.d/ 90-keylightc.rules
