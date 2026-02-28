#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "luxyd-ioctl.h"

#define DEVICE_PATH	"/dev/luxyd_fpga"
#define MEM_ALIGNMENT	4096

#define NC		5000
#define START_BYTES	2048
#define STEP_BYTES	512

#define DMA_ADDR_BASE	0x80000000
#define DMA_ADDR_OFFSET	0x00000000

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

void print_matrix_u32(const char *name, size_t rows, size_t cols, __u32 *matrix) {
	printf("--- %s (%zu x %zu) ---\n", name, rows, cols);

	for (size_t i = 0; i < rows; i++) {
		for (size_t j = 0; j < cols; j++) {
			printf("%7u ", matrix[i * cols + j]);
		}
		printf("\n");
	}
	printf("\n");
}

__u32 *allocate_matrix_u32(size_t rows, size_t cols) {
	void *ptr = NULL;
	size_t size = rows * cols * sizeof(__u32);

	posix_memalign(&ptr, MEM_ALIGNMENT, size);
	memset(ptr, 0, size);

	return (__u32 *)ptr;
}

int main(int argc, char *argv[])
{
	int ret = EXIT_FAILURE;

	printf("%s start\n", argv[0]);

	srand((__u32)time(NULL));

	/* Opening device */
	int fd = open(DEVICE_PATH, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "failed to open %s\n", DEVICE_PATH);
		return fd;
	}

	/* Generate random dimension between 4 and 32 */
	size_t m = 1;//(rand() % 29) + 4;
	size_t n = NC;//(rand() % 29) + 4;
	/* 1020 OK, 1022 NG */

	/* Allocate memory */
	__u32 *A = allocate_matrix_u32(m, n);
	__u32 *A_fpga = allocate_matrix_u32(m, n);

	size_t sizeA = m * n * sizeof(__u32);

	if (!A || !A_fpga) {
		perror("failed to allocate memory\n");
		goto cleanup;
	}

	for (size_t i = 0; i < m * n; i++)
		A[i] = rand() % 256;

	/* Display them */
	//print_hex_dump("A     : ", A, 64);
	//print_hex_dump("A_fpga: ", A_fpga, 64);

	/* Send DMA test config */
	dmatest_config config = {
		.base = DMA_ADDR_BASE,
		.offset = DMA_ADDR_OFFSET
        };

	printf("Sending DMA test offset location...\n");
        ret = ioctl(fd, LUXYD_IOCTL_DMATEST, &config);
        if (ret < 0) {
                perror("failed to send ioctl LUXYD_IOCTL_DMATEST\n");
                goto cleanup;
        }

        printf("DMA test configuration sent\n");

	size_t dsize = START_BYTES;
	do {
		/* Send test data */
		printf("Sending test data (%lu bytes)...\n", dsize);
		ret = write(fd, A, dsize);
		if (ret < 0) {
			perror("failed to write data\n");
			goto cleanup;
		}

		if (ret != dsize) {
			fprintf(stderr, "incomplete: wrote %d of %lu bytes\n",
				ret, dsize);
			ret = -EIO;
			goto cleanup;
		}

		printf("%u bytes of data sent\n", ret);

		/* Read test data */
		printf("Receiving test data (%lu bytes)...\n", dsize);
		ret = read(fd, A_fpga, dsize);
		if (ret < 0) {
			perror("failed to read data\n");
			goto cleanup;
		}

		if (ret != dsize) {
			fprintf(stderr, "incomplete: read %d of %lu bytes\n",
				ret, dsize);
			ret = -EIO;
			goto cleanup;
		}

		printf("%u bytes of data received\n", ret);

		/* Compare result */
		int mismatch = 0;

		for (size_t i = 0; i < dsize / sizeof(__u32); i++) {
			if (A[i] != A_fpga[i]) {
				mismatch = 1;
				break;
			}
		}

		/* Display them */
		//print_hex_dump("A     : ", A, 64);
		//print_hex_dump("A_fpga: ", A_fpga, 64);
		//print_matrix_u32("Matrix A", m, n, A);
		//print_matrix_u32("Matrix A_fpga", m, n, A_fpga);

		if (mismatch) {
			printf("Verification failed: FPGA and CPU results do not matched\n");

			/* Display them */
			print_hex_dump("A     : ", A, 64);
			print_hex_dump("A_fpga: ", A_fpga, 64);

			ret = EXIT_FAILURE;
			goto cleanup;
		} else {
			printf("Verification success: FPGA and CPU results matched\n");
		}

		dsize = dsize + STEP_BYTES;
	} while (dsize <= sizeA);

	ret = EXIT_SUCCESS;

cleanup:
	if (A)
		free(A);

	if (A_fpga)
		free(A_fpga);

	if (fd)
		close(fd);

	printf("%s end\n", argv[0]);
	return ret;
}
