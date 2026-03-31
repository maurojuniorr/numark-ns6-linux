obj-m += snd-ns6-audio.o

# Garante que pegamos o kernel correto do Fedora
KVER := $(shell uname -r)
KDIR := /lib/modules/$(KVER)/build
PWD  := $(shell pwd)
DEST := /lib/modules/$(KVER)/extra

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	sudo mkdir -p $(DEST)
	sudo cp snd-ns6-audio.ko $(DEST)/
	sudo depmod -a
	@echo "----------------------------------------------"
	@echo "Driver instalado em: $(DEST)"
	@echo "Para ativar: sudo modprobe snd-ns6-audio"
	@echo "----------------------------------------------"

.PHONY: all clean install