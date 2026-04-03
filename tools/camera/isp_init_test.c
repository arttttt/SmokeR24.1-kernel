/*
 * ISP init test — call NvIspOpen via stock blob on SmokeR24.1
 *
 * Directly links with libnvisp_v3.so (no dlopen needed).
 *
 * Build: arm-linux-gnueabihf-gcc -std=gnu99 -fPIE -pie \
 *   -Wl,--dynamic-linker=/system/bin/linker \
 *   -L/path/to/libs -lnvisp_v3 -lnvos -lnvrm -lnvrm_graphics \
 *   -o isp_init_test isp_init_test.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* NvIsp function signatures (from reverse engineering) */
typedef uint32_t NvError;
typedef void* NvIspHandle;

/* Declare external functions from libnvisp_v3.so */
extern NvError NvIspOpen(NvIspHandle *phIsp, int ispIndex);
extern NvError NvIspClose(NvIspHandle hIsp);

/* NvRm from libnvrm.so — must be initialized before NvIsp */
typedef void* NvRmDeviceHandle;
extern NvError NvRmOpenNew(NvRmDeviceHandle *phDevice);
extern void NvRmClose(NvRmDeviceHandle hDevice);

int main(int argc, char **argv)
{
	printf("=== ISP Init Test (stock blob on 24.1) ===\n\n");

	/* Initialize NvRm first — required by NvIsp */
	printf("Calling NvRmOpenNew...\n");
	NvRmDeviceHandle hRm = NULL;
	NvError err = NvRmOpenNew(&hRm);
	printf("NvRmOpenNew returned: 0x%x, handle=%p\n", err, hRm);
	if (err != 0) {
		printf("NvRmOpenNew FAILED\n");
		return 1;
	}

	/* Try NvIspOpen for ISP-A (index 0) */
	printf("\nCalling NvIspOpen(0)...\n");
	NvIspHandle hIsp = NULL;
	err = NvIspOpen(&hIsp, 0);
	printf("NvIspOpen returned: 0x%x, handle=%p\n", err, hIsp);

	if (err != 0) {
		printf("NvIspOpen FAILED with error 0x%x\n", err);
		return 1;
	}

	printf("\n*** ISP OPENED SUCCESSFULLY ***\n");
	printf("Handle: %p\n", hIsp);
	printf("\nISP should now be initialized.\n");
	printf("Run: /data/local/tmp/isp_test_24 dma\n");
	printf("to test if DMA works after init.\n");

	/* Keep ISP open if "keep" argument */
	if (argc > 1 && strcmp(argv[1], "keep") == 0) {
		printf("\nKeeping ISP open. Press Enter to close...\n");
		getchar();
	} else {
		printf("Closing ISP...\n");
		NvIspClose(hIsp);
		printf("ISP closed.\n");
	}

	NvRmClose(hRm);
	return 0;
}
