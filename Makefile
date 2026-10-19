# SPDX-License-Identifier: GPL-2.0
#
# Huawei MateBook E camera support - out-of-tree kernel modules + userspace
#
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)
SRC  := $(PWD)/src
CC   ?= cc
CFLAGS ?= -O2 -Wall -Wextra

.PHONY: all module camera clean install uninstall

all: module camera

module:
	$(MAKE) -C $(KDIR) M=$(SRC) modules

camera: apps/gc5024-camera

apps/gc5024-camera: apps/gc5024-camera.c
	$(CC) $(CFLAGS) -o $@ $< -lm

clean:
	$(MAKE) -C $(KDIR) M=$(SRC) clean
	rm -f apps/gc5024-camera

install:
	$(MAKE) -C $(KDIR) M=$(SRC) modules_install
	depmod -A

uninstall:
	rm -f /lib/modules/$(shell uname -r)/extra/gc5024.ko*
	depmod -A
