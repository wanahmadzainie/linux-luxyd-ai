#include <ctype.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linux/types.h>

#define DEVICE_PATH	"/dev/luxyd_fpga"
#define BUFFER_SIZE	(4 * 1024 * 1024)

void print_hex_dump(const char *prefix, const void *buf, size_t len)
{
	const unsigned char *p = (const unsigned char *)buf;

	for (size_t i = 0; i < len; i += 16) {
		printf("%s%08zx: ", prefix, i);

		/* Hex */
		for (size_t j = 0; j < 16; j++) {
			if (i + j < len)
				printf("%02x ", p[i + j]);
			else
				printf("   ");
		}

		printf(" ");

		/* ASCII */
		for (size_t j = 0; j < 16; j++) {
			if (i + j < len)
				printf("%c", isprint(p[i + j]) ? p[i + j] : '.');
		}

		printf("\n");
	}
}

int main(int argc, char *argv[])
{
	int ret = EXIT_FAILURE;
	__u32 *data_out;
	__u32 *data_in;
	size_t count;
	int fd;

	printf("%s start\n", argv[0]);

	/* Allocate memory */
	data_out = malloc(BUFFER_SIZE);
	data_in = malloc(BUFFER_SIZE);
	if (!data_out || !data_in) {
		perror("failed to allocate memory\n");
		goto cleanup;
	}

	memset(data_out, 0, BUFFER_SIZE);
	memset(data_in, 0, BUFFER_SIZE);

	srand(time(NULL));
	for (count = 0; count < BUFFER_SIZE/sizeof(data_in); count++)
		data_in[count] = (__u32)rand();

	/* Opening device */
	fd = open(DEVICE_PATH, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "failed to open %s\n", DEVICE_PATH);
		goto cleanup;
	}

	/* Send data */
	printf("Sending %u bytes of data...\n", BUFFER_SIZE);
	ret = write(fd, data_in, BUFFER_SIZE);
	if (ret < 0) {
		perror("failed to write data\n");
		goto cleanup;
	}

	if (ret != BUFFER_SIZE) {
		fprintf(stderr, "incomplete: wrote %d of %u bytes\n",
			ret, BUFFER_SIZE);
		goto cleanup;
	}

	printf("%u bytes of data sent\n", BUFFER_SIZE);

	/* Read data */
	printf("Receiving %u bytes of data...\n", BUFFER_SIZE);
	ret = read(fd, data_out, BUFFER_SIZE);
	if (ret < 0) {
		perror("failed to read data\n");
		goto cleanup;
	}

	if (ret != BUFFER_SIZE) {
		fprintf(stderr, "incomplete: read %d of %u bytes\n",
			ret, BUFFER_SIZE);
		goto cleanup;
	}

	printf("%u bytes of data received\n", BUFFER_SIZE);

	/* Compare data */
	if (!memcmp(data_in, data_out, BUFFER_SIZE))
		printf("data are identical\n");
	else
		perror("data are not identical\n");

	/* Show data, a bit */
	print_hex_dump("data_in : ", data_in, 16);
	print_hex_dump("data_out: ", data_out, 16);

	ret = EXIT_SUCCESS;

cleanup:
	if (data_out)
		free(data_out);

	if (data_in)
		free(data_in);

	if (fd)
		close(fd);

	printf("%s end\n", argv[0]);
	return ret;
}
