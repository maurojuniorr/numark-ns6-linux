#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};


MODULE_INFO(depends, "snd,snd-pcm");

MODULE_ALIAS("usb:v15E4p0079d*dc*dsc*dp*ic*isc*ip*in01*");

MODULE_INFO(srcversion, "390450F04E5EBB000F8BAB3");
