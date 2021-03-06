
#include <stdio.h>
#include <fcntl.h>
#include <stdint.h>

typedef int rlim64_t;
#include <libzfs.h>
#include <sys/zfs_ioctl.h>

int main(int argc, const char **argv)
{
	int fd;
	zfs_cmd_t zc = { "\0" };

	if (argc < 2) {
		fprintf(stderr, "Usage: %s ZFSNAME\n", argv[0]);
		return -1;
	}

	if ((fd = open("/dev/zfs", O_RDWR)) < 0)
		return -1;

	strcpy(zc.zc_name, argv[1]);
	ioctl(fd, ZFS_IOC_USEROBJSPACE_UPGRADE, &zc);
}
