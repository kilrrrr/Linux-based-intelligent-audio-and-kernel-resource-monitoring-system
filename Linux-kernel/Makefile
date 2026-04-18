ifneq ($(KERNELRELEASE),)
obj-m += music_gpio.o
music_gpio-objs := device.o fops.o
obj-m += res_guard.o
else
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
CC ?= gcc
CFLAGS_res_guardd ?= -Wall -Wextra -O2
CFLAGS_res_guard_test ?= -Wall -Wextra -O2
CFLAGS_res_guardctl ?= -Wall -Wextra -O2
CFLAGS_res_guard_alsa_demo ?= -Wall -Wextra -O2
ALSA_CFLAGS ?=
ALSA_LIBS ?= -lasound -lm

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

res_guardd: res_guardd.c res_guard.h
	$(CC) $(CFLAGS_res_guardd) -o $@ res_guardd.c

res_guard_test: res_guard_test.c res_guard.h
	$(CC) $(CFLAGS_res_guard_test) -o $@ res_guard_test.c

res_guardctl: res_guardctl.c res_guard.h
	$(CC) $(CFLAGS_res_guardctl) -o $@ res_guardctl.c

res_guard_alsa_demo: res_guard_alsa_demo.c res_guard.h
	$(CC) $(CFLAGS_res_guard_alsa_demo) $(ALSA_CFLAGS) -o $@ res_guard_alsa_demo.c $(ALSA_LIBS)

userspace: res_guardd res_guard_test res_guardctl

userspace-alsa: res_guard_alsa_demo

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(RM) res_guardd res_guard_test res_guardctl res_guard_alsa_demo
endif
