	.set noreorder
#include "pspimport.s"
/* Firmware 6.60/6.61 kernel export that boots the Media Engine in an H.264 mode. */
	IMPORT_START "sceMeCore_driver",0x00010000
	IMPORT_FUNC  "sceMeCore_driver",0x5DFF5C50,sceMeBootStart660
