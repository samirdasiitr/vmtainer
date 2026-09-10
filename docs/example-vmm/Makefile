# Minimal Makefile for example-vmm.  Only gcc and make are required.

CC      := gcc
CFLAGS  := -Wall -Wextra -O2 -D_GNU_SOURCE -pthread
LDFLAGS := -pthread

OBJS := vmm.o kernel.o acpi.o bios.o
BIN  := vmm

.PHONY: all clean initrd

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

initrd/bin:
	mkdir -p initrd/bin

initrd/bin/busybox: ../../busybox | initrd/bin
	cp $< $@
	chmod +x $@
	cd initrd/bin && for a in $$(./busybox --list); do \
	    [ "$$a" = "busybox" ] || ln -sf busybox "$$a"; \
	done

initrd.cpio.gz: initrd/init initrd/bin/busybox
	chmod +x initrd/init
	cd initrd && find . -print0 | cpio --null -ov --format=newc 2>/dev/null | gzip > ../$@

initrd: initrd.cpio.gz

clean:
	rm -f $(BIN) $(OBJS) initrd.cpio.gz
	rm -rf initrd/bin
