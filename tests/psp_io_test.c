#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

static off_t next_result;
off_t __real__lseek(int fd, off_t offset, int whence) {
    assert(fd == 7 && offset == 123 && whence == 0);
    if (next_result == (off_t)-1) errno = EBADF;
    return next_result;
}
off_t __wrap__lseek(int fd, off_t offset, int whence);
int main(void) {
    next_result = 123;
    errno = 0;
    assert(__wrap__lseek(7, 123, 0) == 123 && errno == 0);
    next_result = 0;
    assert(__wrap__lseek(7, 123, 0) == 0);
    next_result = -1;
    assert(__wrap__lseek(7, 123, 0) == -1 && errno == EBADF);
    next_result = (int32_t)UINT32_C(0x80020328);
    assert(__wrap__lseek(7, 123, 0) == -1 && errno == EIO);
    next_result = 123;
    assert(__wrap__lseek(7, 123, 0) == 123);
    puts("PSP seek error normalization passed");
}
