/* Import records in the current PSPSDK format, matching exports.exp.
 * psp-fixup-imports resolves these together with SDK imports. */
.set noreorder
#include "pspimport.s"
IMPORT_START "me_lab",0x40090000
IMPORT_FUNC "me_lab",0x3CDFC6FA,meLabInit
IMPORT_FUNC "me_lab",0x5D4EC738,meLabSubmit
IMPORT_FUNC "me_lab",0x9C1A5445,meLabWait
IMPORT_FUNC "me_lab",0x432E5043,meLabShutdown
IMPORT_FUNC "me_lab",0x5B4CD9E2,meLabGetDiagnostics
IMPORT_FUNC "me_lab",0xF7F95032,meLabBackend
