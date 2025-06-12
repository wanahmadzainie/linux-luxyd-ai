#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define LUXYD_AI_DEVICE		"/dev/luxyd-ai"

/* IOCTL commands */
#define LUXYD_AI_IOCTL_MAGIC            'L'
#define LUXYD_AI_LOAD_MODEL             _IOW (LUXYD_AI_IOCTL_MAGIC, 1, unsigned int)
#define LUXYD_AI_START_INFERENCE        _IOWR(LUXYD_AI_IOCTL_MAGIC, 2, unsigned int)
#define LUXYD_AI_GET_STATUS             _IOR (LUXYD_AI_IOCTL_MAGIC, 3, unsigned int)

int main(void)
{
	int fd;
	unsigned int val;
	int ret = 0;

	printf("\n----- LUXYD AI Test Application -----\n");

	printf("[%s] Opening device\n", LUXYD_AI_DEVICE);
	fd = open(LUXYD_AI_DEVICE, O_RDWR);
	if (fd < 0) {
		printf("[%s] Cannot open device file\n", LUXYD_AI_DEVICE);
		return fd;
	}

	printf("[%s] LUXYD_AI_GET_STATUS test start\n", LUXYD_AI_DEVICE);
	ret = ioctl(fd, LUXYD_AI_GET_STATUS, &val);
	if (ret) {
		printf("[%s] LUXYD_AI_GET_STATUS test failed (%d)\n",
		       LUXYD_AI_DEVICE, ret);
		goto err;
	}

	printf("[%s] LUXYD_AI_GET_STATUS test success, received data 0x%08x\n",
	       LUXYD_AI_DEVICE, val);

	printf("[%s] LUXYD_AI_LOAD_MODEL test start\n", LUXYD_AI_DEVICE);
	ret = ioctl(fd, LUXYD_AI_LOAD_MODEL, &val);
	if (ret) {
		printf("[%s] LUXYD_AI_LOAD_MODEL test failed (%d)\n",
		       LUXYD_AI_DEVICE, ret);
		goto err;
	}

	printf("[%s] LUXYD_AI_LOAD_MODEL test success\n", LUXYD_AI_DEVICE);

	printf("[%s] LUXYD_AI_START_INFERENCE test start\n", LUXYD_AI_DEVICE);
	ret = ioctl(fd, LUXYD_AI_START_INFERENCE, &val);
	if (ret) {
		printf("[%s] LUXYD_AI_START_INFERENCE test failed (%d)\n",
		       LUXYD_AI_DEVICE, ret);
		goto err;
	}

	printf("[%s] LUXYD_AI_START_INFERENCE test success\n", LUXYD_AI_DEVICE);

err:
	/* Clean up */
	printf("[%s] Closing device\n", LUXYD_AI_DEVICE);
	close(fd);

	return ret;
}
