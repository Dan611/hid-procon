ifneq ($(KERNELRELEASE),)
	obj-m := hid-procon.o
else
	KERNELDIR  ?= /lib/modules/$(shell uname -r)/build
	INSTALLDIR := /lib/modules/$(shell uname -r)/kernel/drivers/hid
	PWD := $(shell pwd)

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
clean:
	-sudo rmmod ./hid-procon.ko
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
load:
	-sudo rmmod ./hid-procon.ko
	sudo modprobe ff-memless
	sudo insmod ./hid-procon.ko
	sudo cp 10-procon.rules /etc/udev/rules.d/
	sudo udevadm control --reload-rules
	sudo udevadm trigger
unload:
	sudo rmmod ./hid-procon.ko
	sudo rm -f /etc/udev/rules.d/10-procon.rules
	sudo udevadm control --reload-rules
	sudo udevadm trigger
install:
	sudo cp -n ./hid-procon.ko $(INSTALLDIR)
	grep -q -x -F 'hid-procon' /etc/modules || echo hid-procon | sudo tee -a /etc/modules
	grep -q -x -F 'ff-memless' /etc/modules || echo ff-memless | sudo tee -a /etc/modules
	sudo depmod
	sudo cp 10-procon.rules /etc/udev/rules.d/
	sudo udevadm control --reload-rules
	sudo udevadm trigger
uninstall:
	# if both hid-procon and ff-memless are in /etc/modules
	if [ -n "$(shell grep -x -F 'hid-procon' /etc/modules)" ] && [ -n "$(shell grep -x -F 'ff-memless' /etc/modules)" ]; then \
		# and hid-procon comes before ff-memless \
		if [ "$(shell grep -n -x -F 'hid-procon' /etc/modules | head -1 | cut -d : -f 1)" -lt "$(shell grep -n -x -F 'ff-memless' /etc/modules | head -1 | cut -d : -f 1)" ]; then \
			# then remove ff-memless because we added it \
			sudo sed -i '/ff-memless/d' /etc/modules; \
		fi \
	fi
	sudo sed -i '/hid-procon/d' /etc/modules
	sudo rm $(INSTALLDIR)/hid-procon.ko
	sudo depmod
	sudo rm -f /etc/udev/rules.d/10-procon.rules
	sudo udevadm control --reload-rules
	sudo udevadm trigger
endif
