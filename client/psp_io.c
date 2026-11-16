#include <errno.h>
#include <sys/types.h>

/* libcglue converts 0x8001xxxx errors, but can return other negative firmware
 * errors unchanged (e.g. DRIVER_DELETED while storage is suspended). Newlib's
 * seek callbacks require exactly -1 on failure; other values become offsets. */
off_t __real__lseek(int fd, off_t offset, int whence);
off_t __wrap__lseek(int fd, off_t offset, int whence) {
    off_t result = __real__lseek(fd, offset, whence);
    if (result < 0 && result != (off_t)-1) {
        errno = EIO;
        return (off_t)-1;
    }
    return result;
}
