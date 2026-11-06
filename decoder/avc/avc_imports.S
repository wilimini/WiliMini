	.set noreorder
#include "pspimport.s"
/* Client imports for the hardware AVC path. WiliAvc is avc_helper.prx; sceMpegGetAvcNalAu exists
 * only in flash0:/kd/mpeg_vsh.prx, so the loader leaves it unresolved and the helper binds it once
 * the client has loaded that module. */
	IMPORT_START "WiliAvc",0x40090000
	IMPORT_FUNC  "WiliAvc",0xD0D8879F,wiliAvcBootMode
	IMPORT_FUNC  "WiliAvc",0x5B989C5C,wiliAvcFixupImports

	IMPORT_START "sceMpeg",0x00090000
	IMPORT_FUNC  "sceMpeg",0x11F95CF1,sceMpegGetAvcNalAu
