#ifdef __linux__
#include <errno.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_statx
#define SYS_statx 291
#endif

/* Buildroot's musl runtime predates the public statx symbol. Zig's newer musl
 * uses it internally, so provide the kernel syscall ABI in the executable. */
__attribute__((visibility("default"))) int statx(int directory_fd, const char* path, int flags,
                                                 unsigned int mask, void* buffer)
{
	const long result = syscall(SYS_statx, directory_fd, path, flags, mask, buffer);
	if (result < 0)
		return -1;
	return (int)result;
}
#endif
