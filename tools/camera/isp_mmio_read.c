#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>

/* Read ISP registers via single mmap */
int main(int argc, char **argv)
{
	uint32_t base = 0x54680000; /* ISP-B default */
	volatile uint32_t *regs;
	int fd;

	if (argc > 1)
		base = strtoul(argv[1], NULL, 0);

	fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}

	regs = mmap(NULL, 0x4000, PROT_READ, MAP_SHARED, fd, base);
	if (regs == MAP_FAILED) {
		perror("mmap");
		close(fd);
		return 1;
	}

	printf("=== ISP MMIO @ 0x%08x ===\n", base);

	/* Key registers only */
	int keys[] = {
		/* Control */
		0x008, 0x00c, 0x00d, 0x014, 0x015,
		0x018, 0x019, 0x01a, 0x01b, 0x01c, 0x01d, 0x01f,
		0x024, 0x025, 0x026, 0x028, 0x029, 0x02a,
		0x038, 0x03b, 0x03f,
		/* ISP enable + work buf */
		0x051, 0x052, 0x053, 0x054,
		/* Input */
		0x100, 0x101, 0x102, 0x103,
		/* Processing */
		0x500, 0x501, 0x502, 0x503, 0x504, 0x505,
		/* Output */
		0xe00, 0xe01, 0xe02, 0xe03,
		0xe04, 0xe05, 0xe06, 0xe07, 0xe08, 0xe09,
		0xe0a, 0xe0b, 0xe0c,
		/* Output extra */
		0xe30, 0xe31, 0xe32, 0xe33, 0xe34, 0xe35,
		-1
	};

	int i;
	for (i = 0; keys[i] >= 0; i++)
		printf("%03x: 0x%08x\n", keys[i], regs[keys[i]]);

	munmap((void *)regs, 0x4000);
	close(fd);
	return 0;
}
