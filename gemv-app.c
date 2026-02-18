#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>

#include "luxyd-ioctl.h"

#define DEVICE_PATH	"/dev/luxyd_fpga"

/* Function to read parameters from text file (C-style) */
static int read_parameters_c(char const *filename, gemv_config *params)
{
	FILE *file = fopen(filename, "r");

	if (!file) {
		fprintf(stderr, "Error: Could not open parameter file: %s\n",
			filename);
		return 0;
	}

	char line[256];
	while (fgets(line, sizeof(line), file)) {
		char key[64];

		/* Remove trailing newline if present */
		line[strcspn(line, "\n")] = 0;

		if (sscanf(line, "%63[^=]=", key) == 1) { /* Read key up to '=' */
			if (strcmp(key, "n") == 0) {
				sscanf(line + strlen(key) + 1, "%d", &params->n);
			} else if (strcmp(key, "bs") == 0) {
				sscanf(line + strlen(key) + 1, "%zu", &params->bs);
			} else if (strcmp(key, "nr") == 0) {
				sscanf(line + strlen(key) + 1, "%d", &params->nr);
			} else if (strcmp(key, "nc") == 0) {
				sscanf(line + strlen(key) + 1, "%d", &params->nc);
			}
		}
	}

	fclose(file);
	return 1;
}

/* Function to read binary data (C-style) */
static int read_binary_data_c(char const *filename, void *buffer, size_t size)
{
	FILE *file = fopen(filename, "rb");

	if (!file) {
		fprintf(stderr, "Error: Could not open binary data file: %s\n",
			filename);
		return 0;
	}

	size_t bytes_read = fread(buffer, 1, size, file);

	if (bytes_read != size) {
		fprintf(stderr, "Error: Could not read expected amount of data from %s. "
			"Expected: %zu, Read: %zu\n", filename, size, bytes_read);
		fclose(file);
		return 0;
	}

	fclose(file);
	return 1;
}

/* Function to compare calculated output with expected output (C-style) */
static int compare_outputs_c(float const *calculated, float const *expected,
			     int nc, float tolerance)
{
	fprintf(stdout, "\n=== OUTPUT COMPARISON ===\n");
	fprintf(stdout, "Tolerance: %e\n", tolerance);

	int mismatches = 0;
	float max_diff = 0.0f;
	float sum_squared_diff = 0.0f;

	/* Compare each element */
	for (int i = 0; i < nc; i++) {
		float diff = fabsf(calculated[i] - expected[i]);

		sum_squared_diff += diff * diff;

		if (diff > max_diff)
			max_diff = diff;

		if (diff > tolerance) {
			mismatches++;
			if (mismatches <= 10) { /* Show first 10 mismatches */
				fprintf(stdout, "Mismatch at index %d: calculated=%f, "
					"expected=%f, diff=%f\n", i, calculated[i],
					expected[i], diff);
			}
		}
	}

	/* Calculate statistics */
	float rmse = sqrtf(sum_squared_diff / nc);
	float match_percentage = ((float)(nc - mismatches) / nc) * 100.0f;

	fprintf(stdout, "\nComparison Results:\n");
	fprintf(stdout, "  Total elements: %d\n", nc);
	fprintf(stdout, "  Mismatches: %d\n", mismatches);
	fprintf(stdout, "  Match percentage: %.2f%%\n", match_percentage);
	fprintf(stdout, "  Maximum difference: %f\n", max_diff);
	fprintf(stdout, "  Root Mean Square Error (RMSE): %f\n", rmse);

	if (mismatches > 10)
		fprintf(stdout, "  (Showing first 10 mismatches only)\n");

	int passed = (mismatches == 0);

	fprintf(stdout, "\nTest Result: %s\n", (passed ? "PASSED" : "FAILED"));

	return passed;
}

/* --- Helper Functions for main --- */

/* Function to open device and set parameters via ioctl */
static int app_open_and_setup_device(char const *device_path,
				     gemv_config *params_out,
				     size_t *vx_data_size_bytes_out,
				     size_t *vy_data_size_bytes_out)
{
	int fd = open(device_path, O_RDWR);

	if (fd < 0) {
		fprintf(stderr, "Failed to open device %s: %s\n", device_path,
			strerror(errno));
		return -1;
	}
	fprintf(stdout, "Successfully opened device %s\n", device_path);

	/* Read parameters from params.txt */
	memset(params_out, 0, sizeof(gemv_config));
	if (!read_parameters_c("params.txt", params_out)) {
		close(fd);
		return -1;
	}
	fprintf(stdout, "Parameters loaded from params.txt:\n");
	fprintf(stdout, "  n = %d\n", params_out->n);
	fprintf(stdout, "  bs = %zu\n", params_out->bs);
	fprintf(stdout, "  nr = %d\n", params_out->nr);
	fprintf(stdout, "  nc = %d\n", params_out->nc);

	/* Calculate sizes for vx and vy data */
	int const qk = QK_K; /* 256 */
	int const nb = params_out->n / qk;
	int const ncols_interleaved = 8; /* From original helper.hpp */

	*vx_data_size_bytes_out = (size_t)nb * (params_out->nc / ncols_interleaved) *
				  sizeof(block_q4_Kx8_kernel);
	*vy_data_size_bytes_out = (size_t)nb * sizeof(block_q8_K_kernel);

	/* Send combined configuration to driver via ioctl */
	if (ioctl(fd, LUXYD_IOCTL_GEMV, params_out) < 0) {
		fprintf(stderr, "Failed to send LUXYD_IOCTL_GEMV config via ioctl: %s\n",
			strerror(errno));
		close(fd);
		return -1;
	}
	fprintf(stdout, "LUXYD_IOCTL_GEMV config successfully sent to kernel driver.\n");

	return fd;
}

/* Function to transfer vx and vy data to the device using new ioctls */
static int app_transfer_data(int fd,
			     size_t vx_data_size_bytes, void **vx_data_ptr,
			     size_t vy_data_size_bytes, void **vy_data_ptr)
{
	ssize_t bytes_written;

	/* --- 1. Allocate and read vx data from file --- */
	*vx_data_ptr = malloc(vx_data_size_bytes);
	if (!*vx_data_ptr) {
		fprintf(stderr, "Failed to allocate memory for vx_data\n");
		return 1;
	}
	fprintf(stdout, "Reading vx data from vx.bin (size: %zu bytes)...\n",
		vx_data_size_bytes);
	if (!read_binary_data_c("vx.bin", *vx_data_ptr, vx_data_size_bytes))
		return 1;
	fprintf(stdout, "vx data read.\n");

	/* --- 2. Allocate and read vy data from file --- */
	*vy_data_ptr = malloc(vy_data_size_bytes);
	if (!*vy_data_ptr) {
		fprintf(stderr, "Failed to allocate memory for vy_data\n");
		return 1;
	}
	fprintf(stdout, "Reading vy data from vy.bin (size: %zu bytes)...\n",
		vy_data_size_bytes);
	if (!read_binary_data_c("vy.bin", *vy_data_ptr, vy_data_size_bytes))
		return 1;
	fprintf(stdout, "vy data read.\n");

	/* --- 3. Write vx data to the device --- */
	bytes_written = write(fd, *vx_data_ptr, vx_data_size_bytes);
	if (bytes_written < 0 || (size_t)bytes_written != vx_data_size_bytes) {
		fprintf(stderr, "Failed to write vx data to device: %s\n",
			strerror(errno));
		return 1;
	}
	fprintf(stdout, "vx data successfully written to kernel driver.\n");

	/* --- 4. Write vy data to the device --- */
	bytes_written = write(fd, *vy_data_ptr, vy_data_size_bytes);
	if (bytes_written < 0 || (size_t)bytes_written != vy_data_size_bytes) {
		fprintf(stderr, "Failed to write vy data to device: %s\n",
			strerror(errno));
		return 1;
	}
	fprintf(stdout, "vy data successfully written to kernel driver.\n");

	return 0;
}

/* Function to read computed output, convert to float, and prepare for comparison */
static int app_read_and_process_output(int fd, gemv_config const *params,
				       uint32_t **raw_output_ptr,
				       float **calculated_output_ptr)
{
	size_t output_size_bytes; /* No longer obtained via IOCTL */

	/* Calculate output size from params (which came from driver) */
	output_size_bytes = params->nc * sizeof(uint32_t);

	*raw_output_ptr = (uint32_t *)malloc(output_size_bytes);
	if (!*raw_output_ptr) {
		fprintf(stderr, "Failed to allocate memory for raw_output\n");
		return 1;
	}

	size_t total_bytes_read = 0;
	while (total_bytes_read < output_size_bytes) {
		ssize_t bytes_read_now = read(fd, (char *)*raw_output_ptr +
					      total_bytes_read,
					      output_size_bytes - total_bytes_read);
		if (bytes_read_now < 0) {
			fprintf(stderr, "Failed to read output data from device: %s\n",
				strerror(errno));
			return 1;
		}
		if (bytes_read_now == 0) {
			fprintf(stderr, "End of file reached prematurely while reading "
				"output from device.\n");
			break;
		}
		total_bytes_read += bytes_read_now;
	}

	if (total_bytes_read != output_size_bytes) {
		fprintf(stderr, "Error: Read %zu bytes, expected %zu\n",
			total_bytes_read, output_size_bytes);
		return 1;
	}
	fprintf(stdout, "Successfully read %zu bytes of computed output from kernel "
		"driver.\n", total_bytes_read);

	/* Convert raw integer output to float for further processing */
	int nc_output = output_size_bytes / sizeof(uint32_t);

	*calculated_output_ptr = (float *)malloc(nc_output * sizeof(float));
	if (!*calculated_output_ptr) {
		fprintf(stderr, "Failed to allocate memory for calculated_output\n");
		return 1;
	}

	for (int i = 0; i < nc_output; ++i) {
		int fixed_point_val = (int)(*raw_output_ptr)[i]; // Cast u32 to int (signed)
		(*calculated_output_ptr)[i] = (float)fixed_point_val / 100.0f; // Scale back
	}

	return 0;
}

int main()
{
	/* Construct device path */
	char device_path[256];

	snprintf(device_path, sizeof(device_path), "%s", DEVICE_PATH);

	/* Resource tracking for cleanup */
	int fd = -1;
	void *vx_data = NULL;
	void *vy_data = NULL;
	uint32_t *raw_output = NULL;
	float *calculated_output = NULL;
	float *expected_output = NULL;
	FILE *output_file = NULL;
	int comparison_passed = 0; /* Default to failed */

	gemv_config params; /* Declare params here to be passed to app_open_and_setup_device */
	size_t vx_data_size_bytes = 0;
	size_t vy_data_size_bytes = 0;

	/* --- 1. Open the GEMV device and setup parameters --- */
	fd = app_open_and_setup_device(device_path, &params,
				       &vx_data_size_bytes, &vy_data_size_bytes);
	if (fd < 0)
		goto cleanup;

	/* --- 2. Transfer data (vx and vy) to the device --- */
	if (app_transfer_data(fd, vx_data_size_bytes, &vx_data,
			      vy_data_size_bytes, &vy_data) != 0)
		goto cleanup;

	/* --- 3. Read computed output from the device and process --- */
	if (app_read_and_process_output(fd, &params, &raw_output, &calculated_output) != 0)
		goto cleanup;

	/* --- 4. Save result to result.txt --- */
	output_file = fopen("result.txt", "w");
	if (!output_file) {
		fprintf(stderr, "Error: Could not open result file: result.txt\n");
		goto cleanup;
	}
	for (int i = 0; i < params.nc; ++i)
		fprintf(output_file, "%f\n", calculated_output[i]);

	fclose(output_file);
	output_file = NULL; /* Mark as closed */
	fprintf(stdout, "Computed results saved to result.txt\n");

	/* --- 5. Compare with expected output --- */
	expected_output = (float *)malloc(params.nc * sizeof(float));
	if (!expected_output) {
		fprintf(stderr, "Failed to allocate memory for expected_output\n");
		goto cleanup;
	}
	fprintf(stdout, "Reading expected output from expected_output.bin (size: %zu bytes)...\n",
		params.nc * sizeof(float));
	if (!read_binary_data_c("expected_output.bin", expected_output,
				 params.nc * sizeof(float)))
		goto cleanup;

	comparison_passed = compare_outputs_c(calculated_output, expected_output,
					      params.nc, 1e-6f);

cleanup:
	/* --- Clean up --- */
	if (output_file)
		fclose(output_file);
	if (expected_output)
		free(expected_output);
	if (calculated_output)
		free(calculated_output);
	if (raw_output)
		free(raw_output);
	if (vy_data)
		free(vy_data);
	if (vx_data)
		free(vx_data);
	if (fd != -1)
		close(fd);

	return comparison_passed ? 0 : 1;
}
