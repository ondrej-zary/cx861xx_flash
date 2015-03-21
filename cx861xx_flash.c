#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>

#define VERSION "1.1"

#define CONEXANT_VENDOR		0x0572
#define CX861XX_BOOT_PROD	0xCAFC
#define CX82XXX_BOOT_PROD	0xCAFD

#define CX_EP_CMD	0x01	/* Bulk/interrupt in/out */
#define CMD_TIMEOUT     100	/* msecs */

/* CX861xx memory map:
 0x00000000: either internal ROM (boot loader mode) or external flash (normal boot) mapped here
 0x00400000: 32KB internal ROM (boot loader)
 0x00600000: 1MB I/O (registers and devices)
 0x00800000: 64KB internal SRAM
 0x04000000: FLASH (disabled in boot loader mode)
 0x08000000: SDRAM (disabled on boot)
*/
#define CX861XX_IO_BASE		0x00600000
#define CX861XX_FLASH_ENABLE	(CX861XX_IO_BASE + 4)

/* CX82xxx memory map:
 0x00000000: either internal ROM (boot loader mode) or external flash (normal boot) mapped here
 0x00180000: 32KB internal SRAM (with running copy of boot loader)
 0x00300000: I/O (registers and devices)
 0x00400000: FLASH (always enabled)
 0x00800000: SDRAM (disabled on boot, controlled by bit 0 of EMCR byte at 0x00350010)
*/

#define u8  unsigned char
#define u16 unsigned short
#define u32 unsigned int

u32 flash_base;

enum cx_fw_cmd {
	FW_CMD_ERR,
	FW_GET_VER,
	FW_READ_MEM,
	FW_WRITE_MEM,
	FW_RMW_MEM,
	FW_CHECKSUM_MEM,
	FW_GOTO_MEM,
};

enum cx_mem_access {
	MA_BYTE,
	MA_WORD,
	MA_DWORD
};

/* 64B packet */
struct cx_fw_packet {
	u8 cmd;
	u8 byte_count;	/* max. 56 */
	u8 access_type;
	u8 ack_request;
	u32 address;
	u8 data[56];
} __attribute__((packed));

/********** memory access **********/

int cx_read_mem(libusb_device_handle *dev, u32 addr, u32 count, u8 *buf, u8 access_type) {
	struct cx_fw_packet packet;
	int transferred, ret;

	while (count > 0) {
		memset(&packet, 0, sizeof(packet));
		packet.cmd = FW_READ_MEM;
		if (count > sizeof(packet.data))
			packet.byte_count = sizeof(packet.data); /* 56 */
		else
			packet.byte_count = count;
		u8 packet_byte_count = packet.byte_count;
		packet.access_type = access_type;
		packet.ack_request = 1;
		packet.address = addr;
		ret = libusb_bulk_transfer(dev, CX_EP_CMD, (void *)&packet, sizeof(packet), &transferred, CMD_TIMEOUT);
		if (ret < 0) {
			fprintf(stderr, "Error sending USB request: %d\n", ret);
			return ret;
		}

		memset(&packet, 0, sizeof(packet));
		while (packet_byte_count > 0) {
			ret = libusb_bulk_transfer(dev, CX_EP_CMD | LIBUSB_ENDPOINT_IN, (void *)&packet, sizeof(packet), &transferred, CMD_TIMEOUT);
			if (ret < 0) {
				fprintf(stderr, "Error reading data: %d\n", ret);
				return ret;
			}
			memcpy(buf, packet.data, packet.byte_count);
			buf += packet.byte_count;
			packet_byte_count -= packet.byte_count;
			count -= packet.byte_count;
			addr += packet.byte_count;
		}
		/* print dot each 1 KB */
		if (count >= 1024 && count % 1024 == 0) {
			printf(".");
			fflush(stdout);
		}
	}

	return 0;
}

int cx_write_mem(libusb_device_handle *dev, u32 addr, u32 count, u8 *buf, u8 access_type) {
	struct cx_fw_packet packet;
	int transferred, ret;

	while (count > 0) {
		memset(&packet, 0, sizeof(packet));
		packet.cmd = FW_WRITE_MEM;
		if (count > sizeof(packet.data))
			packet.byte_count = sizeof(packet.data); /* 56 */
		else
			packet.byte_count = count;
		packet.access_type = access_type;
		packet.ack_request = 0;
		packet.address = addr;
		memcpy(packet.data, buf, packet.byte_count);
		ret = libusb_bulk_transfer(dev, CX_EP_CMD, (void *)&packet, sizeof(packet), &transferred, CMD_TIMEOUT);
		if (ret < 0) {
			fprintf(stderr, "Error sending USB request: %d\n", ret);
			return ret;
		}
		buf += packet.byte_count;
		count -= packet.byte_count;
		addr += packet.byte_count;
	}

	return 0;
}

/********** flash access **********/

#define FLASH_CMD_ID		0x90	/* Read Identifier */
#define FLASH_CMD_CFI		0x98	/* CFI Query */

struct block_desc {
	u32 count;
	u32 size;
};

struct flash_chip {
	u16 mfg;
	u16 dev;
	char name[32];
	u32 size;
	struct block_desc blocks[16];
	void (*set_block_lock)(libusb_device_handle *dev, u32 addr, bool lock);
	bool (*erase_block)(libusb_device_handle *dev, u32 addr);
	bool (*program_block)(libusb_device_handle *dev, u32 addr, u16 *data, u32 size, bool slow);
};

u16 cx_flash_read(libusb_device_handle *dev, u32 addr) {
	u16 data;

	cx_read_mem(dev, flash_base + addr, 2, (void *)&data, MA_WORD);

	return data;
}

void cx_flash_write(libusb_device_handle *dev, u32 addr, u16 data) {
	cx_write_mem(dev, flash_base + addr, 2, (void *)&data, MA_WORD);
}

void cx_flash_cmd(libusb_device_handle *dev, u8 cmd) {
	/* flash access is strictly 16-bit
	   As the CPU does not have address line 0 (HC00), flash address lines are shifted by one
	   (A0 is connected to HC01, A1 to HC02...). This means that all addresses must be shifted left by 1.
	*/
	cx_flash_write(dev, 0xaaa, 0xaa); /* 0x555 */
	cx_flash_write(dev, 0x554, 0x55); /* 0x2aa */
	cx_flash_write(dev, 0xaaa, cmd);  /* 0x555 */
}

/********** Intel flash **********/

#define INTEL_CMD_READ		0xff	/* Read Array */
#define INTEL_CMD_READSTATUS	0x70	/* Read Status Register */
#define INTEL_CMD_CLEARSTATUS	0x50	/* Clear Status Register */
#define INTEL_CMD_PROGRAM	0x40	/* Program */
#define INTEL_CMD_ERASE		0x20	/* Block Erase */
#define INTEL_CMD_ERASECONFIRM	0xd0	/* Block Erase Confirm */
#define INTEL_CMD_SUSPEND	0xb0	/* Program/Erase Suspend */
#define INTEL_CMD_RESUME	0xd0	/* Program/Erase Resume */
#define INTEL_CMD_LOCKMODE	0x60	/* Lock mode, use with next 3 commands: */
#define INTEL_CMD_LOCK		0x01	/* Lock Block */
#define INTEL_CMD_UNLOCK	0xd0	/* Unlock Block */
#define INTEL_CMD_LOCKDOWN	0x2f	/* Lock-Down Block */
#define INTEL_CMD_PROT		0xc0	/* Protection Program */

#define INTEL_ST_READY		(1 << 7)	/* Write State Machine Status, 1 = READY */
#define INTEL_ST_ERASESUSPEND	(1 << 6)	/* Erase-Suspend Status, 1 = SUSPENDED */
#define INTEL_ST_ERASEERROR	(1 << 5)	/* Erase Status, 1 = ERROR */
#define INTEL_ST_PROGRAMERROR	(1 << 4)	/* Program Status, 1 = ERROR */
#define INTEL_ST_VPPERROR	(1 << 3)	/* VPP Status, 1 = VPP Low */
#define INTEL_ST_PROGRAMSUSPEND (1 << 2)	/* Program-Suspend Status, 1 = SUSPENDED */
#define INTEL_ST_LOCKED		(1 << 1)	/* Block Lock Status, 1 = LOCKED */

#define INTEL_ST_ERROR_MASK	0x5a		/* erase, program, vpp, lock status */

void intel_decode_status(u16 status) {
	printf("Status decode: ");
	if (status & INTEL_ST_READY)
		printf("READY ");
	if (status & INTEL_ST_ERASESUSPEND)
		printf("ERASE_SUSPEND ");
	if (status & INTEL_ST_ERASEERROR)
		printf("ERASE_ERROR ");
	if (status & INTEL_ST_PROGRAMERROR)
		printf("PROGRAM_ERROR ");
	if (status & INTEL_ST_VPPERROR)
		printf("VPP_ERROR ");
	if (status & INTEL_ST_PROGRAMSUSPEND)
		printf("PROGRAM_SUSPEND ");
	if (status & INTEL_ST_LOCKED)
		printf("LOCKED ");
	printf("\n");
}

bool intel_erase_block(libusb_device_handle *dev, u32 addr) {
	u16 status;
	u32 i = 0;

	printf("Erasing block     0x%06x: ", addr);
	fflush(stdout);
	cx_flash_write(dev, 0, INTEL_CMD_CLEARSTATUS);
	cx_flash_write(dev, 0, INTEL_CMD_READSTATUS);

	cx_flash_write(dev, addr, INTEL_CMD_ERASE);
	cx_flash_write(dev, addr, INTEL_CMD_ERASECONFIRM);
	do {
		status = cx_flash_read(dev, 0);
		if (i % 4 == 0) {
			printf(".");
			fflush(stdout);
		}
		i++;
	} while (!(status & INTEL_ST_READY));

	cx_flash_write(dev, 0, INTEL_CMD_READ);

	if (status & INTEL_ST_ERROR_MASK) {
		printf("Error!\n");
		intel_decode_status(status);
		return false;
	}
	printf("\n");

	return true;
}

void intel_optimized_program_word(libusb_device_handle *dev, u32 addr, u16 data) {
	u16 buf[2];

	/* merge PROGRAM command and data into one write */
	buf[0] = INTEL_CMD_PROGRAM;
	buf[1] = data;
	cx_write_mem(dev, flash_base + addr - 2, 4, (void *)buf, MA_WORD);
}

bool intel_program_block(libusb_device_handle *dev, u32 addr, u16 *data, u32 size, bool slow) {
	u16 status;
	u32 i;

	printf("Programming block 0x%06x: ", addr);
	fflush(stdout);
	cx_flash_write(dev, 0, INTEL_CMD_CLEARSTATUS);
	cx_flash_write(dev, 0, INTEL_CMD_READSTATUS);

	/* Program each 16-byte word */
	for (i = 0; i < size / 2; i++) {
		/* don't program FFFF words */
		if (data[i] == 0xffff)
			continue;

		if (i % 512 == 0) {	/* each 1 KB */
			printf(".");
			fflush(stdout);
		}

		if (i == 0) {
			/* first word can't be optimized */
			cx_flash_write(dev, addr, INTEL_CMD_PROGRAM);
			cx_flash_write(dev, addr + i * 2, data[i]);
		} else
			intel_optimized_program_word(dev, addr + i * 2, data[i]);

		/* USB is so slow that we don't need to wait for programming to end */
		if (slow) {
			/* But it might be useful so it's an option */
			do {
				status = cx_flash_read(dev, 0);
			} while (!(status & INTEL_ST_READY));

			if (status & INTEL_ST_ERROR_MASK) {
				printf("Error!\n");
				intel_decode_status(status);
				cx_flash_write(dev, 0, INTEL_CMD_READ);
				return false;
			}
		}
	}

	cx_flash_write(dev, 0, INTEL_CMD_READ);
	printf("\n");

	return true;
}

void intel_set_block_lock(libusb_device_handle *dev, u32 addr, bool lock) {
	cx_flash_write(dev, addr, INTEL_CMD_LOCKMODE);
	cx_flash_write(dev, addr, lock ? INTEL_CMD_LOCK : INTEL_CMD_UNLOCK);
}

/********** AMD flash **********/

#define AMD_CMD_RESET		0xf0	/* Reset */
#define AMD_CMD_PROGRAM		0xa0	/* Program */
#define AMD_CMD_ERASE_PREPARE	0x80	/* Chip/Sector Erase Prepare*/
#define AMD_CMD_ERASE_CHIP	0x10	/* Chip Erase Confirm */
#define AMD_CMD_ERASE_SECTOR	0x30	/* Sector Erase Confirm */
#define AMD_CMD_SUSPEND		0xb0	/* Sector Erase Suspend */
#define AMD_CMD_RESUME		0x30	/* Sector Erase Resume */
#define AMD_CMD_UNLOCK_BYPASS	0x20	/* Unlock Bypass */
#define AMD_CMD_BYPASS_RESET1	0x90	/* Unlock Bypass Reset (1st) */
#define AMD_CMD_BYPASS_RESET2	0x00	/* Unlock Bypass Reset (2nd) */

#define AMD_ST_DATAPOLL		(1 << 7)	/* DATA polling */
#define AMD_ST_TIMEOUT		(1 << 5)	/* Exceeded Timing Limits */

bool amd_erase_block(libusb_device_handle *dev, u32 addr) {
	u16 status;
	u32 i = 0;

	printf("Erasing block     0x%06x: ", addr);
	fflush(stdout);

	cx_flash_cmd(dev, AMD_CMD_ERASE_PREPARE);
	cx_flash_write(dev, 0xaaa, 0xaa); /* 0x555 */
	cx_flash_write(dev, 0x554, 0x55); /* 0x2aa */
	cx_flash_write(dev, addr, AMD_CMD_ERASE_SECTOR);

	/* DATA polling */
	do {
		status = cx_flash_read(dev, addr);
		if (!(status & AMD_ST_DATAPOLL) && (status & AMD_ST_TIMEOUT)) {
			printf("Error: TIMEOUT!\n");
			cx_flash_write(dev, 0, AMD_CMD_RESET);
			return false;
		}
		if (i % 4 == 0) {
			printf(".");
			fflush(stdout);
		}
		i++;
	} while (!(status & AMD_ST_DATAPOLL));

	printf("\n");

	return true;
}

void amd_optimized_program_word(libusb_device_handle *dev, u32 addr, u16 data) {
	u16 buf[2];

	/* merge PROGRAM command and data into one write */
	buf[0] = AMD_CMD_PROGRAM;
	buf[1] = data;
	cx_write_mem(dev, flash_base + addr - 2, 4, (void *)buf, MA_WORD);
}

bool amd_program_block(libusb_device_handle *dev, u32 addr, u16 *data, u32 size, bool slow) {
	u16 status;
	u32 i;

	printf("Programming block 0x%06x: ", addr);
	fflush(stdout);

	cx_flash_cmd(dev, AMD_CMD_UNLOCK_BYPASS);
	/* Program each 16-byte word */
	for (i = 0; i < size / 2; i++) {
		/* don't program FFFF words */
		if (data[i] == 0xffff)
			continue;

		if (i % 512 == 0) {	/* each 1 KB */
			printf(".");
			fflush(stdout);
		}

		if (i == 0) {
			/* first word can't be optimized */
			cx_flash_write(dev, addr, AMD_CMD_PROGRAM);
			cx_flash_write(dev, addr + i * 2, data[i]);
		} else
			amd_optimized_program_word(dev, addr + i * 2, data[i]);

		/* USB is so slow that we don't need to wait for programming to end */
		if (slow) {
			/* But it might be useful so it's an option */
			/* DATA polling */
			do {
				status = cx_flash_read(dev, addr + i * 2);
				if (status != data[i] && (status & AMD_ST_TIMEOUT)) {
					printf("Error: TIMEOUT!\n");
					cx_flash_write(dev, 0, AMD_CMD_BYPASS_RESET1);
					cx_flash_write(dev, 0, AMD_CMD_BYPASS_RESET2);
					cx_flash_write(dev, 0, AMD_CMD_RESET);
					return false;
				}
			} while (status != data[i]);
		}
	}
	cx_flash_write(dev, 0, AMD_CMD_BYPASS_RESET1);
	cx_flash_write(dev, 0, AMD_CMD_BYPASS_RESET2);

	printf("\n");

	return true;
}

/********** flash parameters **********/

struct flash_chip supported_chips[] = {
	{
		.mfg = 0x0089, .dev = 0x88c5, .name = "Intel 28F320C3B", .size = 4*1024*1024,
		.blocks = {
				{ .count = 8,	.size = 8192 },
				{ .count = 63,	.size = 65536 },
				{ .count = 0 }	/* end marker */
		},
		.set_block_lock = intel_set_block_lock,
		.erase_block = intel_erase_block,
		.program_block = intel_program_block,
	},
	{
		.mfg = 0x00c2, .dev = 0x2249, .name = "MXIC MX29LV160B", .size = 2*1024*1024,
		.blocks = {
				{ .count = 1,	.size = 16384 },
				{ .count = 2,	.size = 8192 },
				{ .count = 1,	.size = 32768 },
				{ .count = 31,	.size = 65536 },
				{ .count = 0 }	/* end marker */
		},
		.erase_block = amd_erase_block,
		.program_block = amd_program_block,
	},
	{ .mfg = 0 }	/* end marker */
};

struct flash_chip *flash_identify(libusb_device_handle *dev) {
	struct flash_chip *flash = NULL;

	/* Send READ IDENTIFIER command */
	cx_flash_cmd(dev, FLASH_CMD_ID);

	/* Read IDs */
	u16 flash_mfg = cx_flash_read(dev, 0);
	u16 flash_dev = cx_flash_read(dev, 2);

	/* Send READ ARRAY command (exit identifier mode) */
	cx_flash_write(dev, 0, INTEL_CMD_READ);
	cx_flash_write(dev, 0, AMD_CMD_RESET);

	printf("Flash ID: Mfg ID=0x%04x, Device ID=0x%04x: ", flash_mfg, flash_dev);
	for (int i = 0; supported_chips[i].mfg != 0; i++)
		if (supported_chips[i].mfg == flash_mfg &&
		    supported_chips[i].dev == flash_dev) {
			flash = &supported_chips[i];
			break;
		}

	return flash;
}

void usage() {
	printf("Usage: cx861xx_flash read|write|writeslow FILE\n");
	printf(" read      = read from flash into FILE\n");
	printf(" write     = write from FILE to flash\n");
	printf(" writeslow = write from FILE to flash, check status after each word\n");
}

int main(int argc, char *argv[])
{
	FILE *f;
	bool cx861xx = false;

	printf("cx861xx_flash v%s - Conexant CX861xx and CX82xxx USB Boot Flash Utility\n", VERSION);
	printf("Copyright (c) 2015 Ondrej Zary - http://www.rainbow-software.org\n\n");

	libusb_init(NULL);
	libusb_set_debug(NULL, 3);

	libusb_device_handle *dev = libusb_open_device_with_vid_pid(NULL, CONEXANT_VENDOR, CX861XX_BOOT_PROD);
	if (dev)
		cx861xx = true;
	else
		dev = libusb_open_device_with_vid_pid(NULL, CONEXANT_VENDOR, CX82XXX_BOOT_PROD);
	if (!dev) {
		fprintf(stderr, "No device detected. Make sure the board is properly connected and processor is in USB Boot mode.\n");
		exit(1);
	}
	flash_base = cx861xx ? 0x04000000 : 0x0400000;
	printf("%s device found at bus %d, address %d\n\n", cx861xx ? "CX861xx" : "CX82xxx",
							    libusb_get_bus_number(libusb_get_device(dev)),
							    libusb_get_device_address(libusb_get_device(dev)));

	int err = libusb_claim_interface(dev, 0);
	if (err) {
		fprintf(stderr, "Unable to claim interface: ");
		switch (err) {
		case LIBUSB_ERROR_NOT_FOUND:	fprintf(stderr, "not found\n");	break;
		case LIBUSB_ERROR_BUSY:		fprintf(stderr, "busy\n");	break;
		case LIBUSB_ERROR_NO_DEVICE:	fprintf(stderr, "no device\n");	break;
		default:			fprintf(stderr, "%d\n", err);	break;
		}
		exit(2);
	}

	if (argc < 3) {
		usage();
		exit(3);
	}

	if (cx861xx) {	/* Enable FLASH access */
		u8 data = 1;
		cx_write_mem(dev, CX861XX_FLASH_ENABLE, 1, &data, MA_BYTE);
	}

	/* Send READ ARRAY command to reset flash */
	cx_flash_write(dev, 0, INTEL_CMD_READ);
	cx_flash_write(dev, 0, AMD_CMD_RESET);

	struct flash_chip *flash = flash_identify(dev);
	if (!flash) {
		printf("Unsupported flash type\n");
		exit(6);
	}
	printf("%s\n", flash->name);
	struct block_desc *flash_blocks = flash->blocks;

	u8 *buf = malloc(flash->size);
	if (!buf) {
		fprintf(stderr, "Memory allocation error, %d bytes required.\n", flash->size);
		exit(4);
	}

	if (!strcmp(argv[1], "write") || !strcmp(argv[1], "writeslow")) {
		bool slow = false;
		if (!strcmp(argv[1], "writeslow"))
			slow = true;

		f = fopen(argv[2], "r");
		if (!f) {
			perror("Error opening file");
			exit(5);
		}

		u32 len = fread(buf, 1, flash->size, f);
		if (len < flash->size) {
			fprintf(stderr, "Error reading file, must be %d bytes long\n", flash->size);
			exit(5);
		}

		u32 addr = 0;
		for (u32 i = 0; flash_blocks[i].count > 0; i++) {
			for (u32 j = 0; j < flash_blocks[i].count; j++) {
				if (flash->set_block_lock)
					flash->set_block_lock(dev, addr, false);
				flash->erase_block(dev, addr);
				flash->program_block(dev, addr, (void *) buf + addr, flash_blocks[i].size, slow);
				if (flash->set_block_lock)
					flash->set_block_lock(dev, addr, true);
				addr += flash_blocks[i].size;
			}
		}
	} else if (!strcmp(argv[1], "read")) {
		f = fopen(argv[2], "w");
		if (!f) {
			perror("Error opening file");
			exit(5);
		}

		printf("Reading flash: ");

		cx_read_mem(dev, flash_base, flash->size, buf, MA_WORD);
		printf("done\n");
		if (fwrite(buf, 1, flash->size, f) != flash->size) {
			fprintf(stderr, "Error writing file\n");
			exit(5);
		}
	} else {
		usage();
		exit(3);
	}

	fclose(f);

	libusb_close(dev);
	libusb_exit(NULL);

	return 0;
}
