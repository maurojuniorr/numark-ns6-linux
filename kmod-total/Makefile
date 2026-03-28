obj-m := snd-ns6.o

KDIR := /lib/modules/$(shell uname -r)/build

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean

install:
	mkdir -p /lib/modules/$(shell uname -r)/extra
	cp snd-ns6.ko /lib/modules/$(shell uname -r)/extra/
	depmod -a
	@echo "----------------------------------------------"
	@echo "Driver instalado: /lib/modules/$(shell uname -r)/extra"
	@echo "Para ativar: sudo modprobe snd-ns6"
	@echo "----------------------------------------------"
