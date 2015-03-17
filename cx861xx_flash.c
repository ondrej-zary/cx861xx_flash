#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>

#define VERSION "1.0"

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

/* Intel 28F320C3B definitions */
#define FLASH_SIZE	0x400000

#define FLASH_CMD_READ		0xff	/* Read Array */
#define FLASH_CMD_ID		0x90	/* Read Identifier */
#define FLASH_CMD_CFI		0x98	/* CFI Query */
#define FLASH_CMD_READSTATUS	0x70	/* Read Status Register */
#define FLASH_CMD_CLEARSTATUS	0x50	/* Clear Status Register */
#define FLASH_CMD_PROGRAM	0x40	/* Program */
#define FLASH_CMD_ERASE		0x20	/* Block Erase */
#define FLASH_CMD_ERASECONFIRM	0xd0	/* Block Erase Confirm */
#define FLASH_CMD_SUSPEND	0xb0	/* Program/Erase Suspend */
#define FLASH_CMD_RESUME	0xd0	/* Program/Erase Resume */
#define FLASH_CMD_LOCKMODE	0x60	/* Lock mode, use with next 3 commands: */
#define FLASH_CMD_LOCK		0x01	/* Lock Block */
#define FLASH_CMD_UNLOCK	0xd0	/* Unlock Block */
#define FLASH_CMD_LOCKDOWN	0x2f	/* Lock-Down Block */
#define FLASH_CMD_PROT		0xc0	/* Protection Program */

#define FLASH_ST_READY		(1 << 7)	/* Write State Machine Status, 1 = READY */
#define FLASH_ST_ERASESUSPEND	(1 << 6)	/* Erase-Suspend Status, 1 = SUSPENDED */
#define FLASH_ST_ERASEERROR	(1 << 5)	/* Erase Status, 1 = ERROR */
#define FLASH_ST_PROGRAMERROR	(1 << 4)	/* Program Status, 1 = ERROR */
#define FLASH_ST_VPPERROR	(1 << 3)	/* VPP Status, 1 = VPP Low */
#define FLASH_ST_PROGRAMSUSPEND (1 << 2)	/* Program-Suspend Status, 1 = SUSPENDED */
#define FLASH_ST_LOCKED		(1 << 1)	/* Block Lock Status, 1 = LOCKED */

#define FLASH_ST_ERROR_MASK	0x5a		/* erase, program, vpp, lock status */

struct block_desc {
	u32 count;
	u32 size;
};

/* Intel 28F320C3B memory map */
struct block_desc flash_blocks[] = {
	{ .count = 8,	.size = 8192 },
	{ .count = 63,	.size = 65536 },
	{ .count = 0 }	/* end marker */
};

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

u16 cx_flash_read(libusb_device_handle *dev, u32 addr) {
	u16 data;

	cx_read_mem(dev, flash_base + addr, 2, (void *)&data, MA_WORD);

	return data;
}

void cx_flash_write(libusb_device_handle *dev, u32 addr, u16 data) {
	cx_write_mem(dev, flash_base + addr, 2, (void *)&data, MA_WORD);
}

void flash_decode_status(u16 status) {
	printf("Status decode: ");
	if (status & FLASH_ST_READY)
		printf("READY ");
	if (status & FLASH_ST_ERASESUSPEND)
		printf("ERASE_SUSPEND ");
	if (status & FLASH_ST_ERASEERROR)
		printf("ERASE_ERROR ");
	if (status & FLASH_ST_PROGRAMERROR)
		printf("PROGRAM_ERROR ");
	if (status & FLASH_ST_VPPERROR)
		printf("VPP_ERROR ");
	if (status & FLASH_ST_PROGRAMSUSPEND)
		printf("PROGRAM_SUSPEND ");
	if (status & FLASH_ST_LOCKED)
		printf("LOCKED ");
	printf("\n");
}

bool flash_erase_block(libusb_device_handle *dev, u32 addr) {
	u16 status;
	u32 i = 0;

	printf("Erasing block     0x%06x: ", addr);
	fflush(stdout);
	cx_flash_write(dev, 0, FLASH_CMD_CLEARSTATUS);
	cx_flash_write(dev, 0, FLASH_CMD_READSTATUS);

	cx_flash_write(dev, addr, FLASH_CMD_ERASE);
	cx_flash_write(dev, addr, FLASH_CMD_ERASECONFIRM);
	do {
		status = cx_flash_read(dev, 0);
		if (i % 4 == 0) {
			printf(".");
			fflush(stdout);
		}
		i++;
	} while (!(status & FLASH_ST_READY));

	cx_flash_write(dev, 0, FLASH_CMD_READ);

	if (status & FLASH_ST_ERROR_MASK) {
		printf("Error!\n");
		flash_decode_status(status);
		return false;
	}
	printf("\n");

	return true;
}

void flash_optimized_program_word(libusb_device_handle *dev, u32 addr, u16 data) {
	u16 buf[2];

	/* merge PROGRAM command and data into one write */
	buf[0] = FLASH_CMD_PROGRAM;
	buf[1] = data;
	cx_write_mem(dev, flash_base + addr - 2, 4, (void *)buf, MA_WORD);
}

bool flash_program_block(libusb_device_handle *dev, u32 addr, u16 *data, u32 size, bool slow) {
	u16 status;
	u32 i;

	printf("Programming block 0x%06x: ", addr);
	fflush(stdout);
	cx_flash_write(dev, 0, FLASH_CMD_CLEARSTATUS);
	cx_flash_write(dev, 0, FLASH_CMD_READSTATUS);

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
			cx_flash_write(dev, addr, FLASH_CMD_PROGRAM);
			cx_flash_write(dev, addr + i * 2, data[i]);
		} else
			flash_optimized_program_word(dev, addr + i * 2, data[i]);

		/* USB is so slow that we don't need to wait for programming to end */
		if (slow) {
			/* But it might be useful so it's an option */
			do {
				status = cx_flash_read(dev, 0);
			} while (!(status & FLASH_ST_READY));

			if (status & FLASH_ST_ERROR_MASK) {
				printf("Error!\n");
				flash_decode_status(status);
				cx_flash_write(dev, 0, FLASH_CMD_READ);
				return false;
			}
		}
	}

	cx_flash_write(dev, 0, FLASH_CMD_READ);
	printf("\n");

	return true;
}

void flash_set_block_lock(libusb_device_handle *dev, u32 addr, bool lock) {
	cx_flash_write(dev, addr, FLASH_CMD_LOCKMODE);
	cx_flash_write(dev, addr, lock ? FLASH_CMD_LOCK : FLASH_CMD_UNLOCK);
}

void flash_identify(libusb_device_handle *dev) {
	/* Send READ IDENTIFIER command */
	cx_flash_write(dev, 0, FLASH_CMD_ID);

	/* Read IDs */
	u16 flash_mfg = cx_flash_read(dev, 0);
	u16 flash_dev = cx_flash_read(dev, 2);

	/* Send READ ARRAY command (exit identifier mode) */
	cx_flash_write(dev, 0, FLASH_CMD_READ);

	printf("Flash ID: Mfg ID=0x%04x, Device ID=0x%04x: ", flash_mfg, flash_dev);
	if (flash_mfg == 0x0089 && flash_dev == 0x88c5)
		printf("Intel 28F320C3B\n");
	else {
		printf("Unsupported flash type\n");
		exit(6);
	}
}

void usage() {
	printf("Usage: cx861xx_flash read|write|writeslow FILE\n");
	printf(" read      = read from flash into FILE\n");
	printf(" write     = write from FILE to flash\n");
	printf(" writeslow = write from FILE to flash, check status after each word\n");
}

#define BUF_SIZE FLASH_SIZE

int main(int argc, char *argv[])
{
	FILE *f;
	bool cx861xx = false;

	printf("cx861xx_flash v%s - Conexant CX861xx USB Boot Flash Utility\n", VERSION);
	printf("Copyright (c) 2012 Ondrej Zary - http://www.rainbow-software.org\n\n");

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

	u8 *buf = malloc(BUF_SIZE);
	if (!buf) {
		fprintf(stderr, "Memory allocation error, %d bytes required.\n", BUF_SIZE);
		exit(4);
	}

	if (cx861xx) {	/* Enable FLASH access */
		u8 data = 1;
		cx_write_mem(dev, CX861XX_FLASH_ENABLE, 1, &data, MA_BYTE);
	}

	/* Send READ ARRAY command to reset flash */
	cx_flash_write(dev, 0, FLASH_CMD_READ);

	flash_identify(dev);

	if (!strcmp(argv[1], "write") || !strcmp(argv[1], "writeslow")) {
		bool slow = false;
		if (!strcmp(argv[1], "writeslow"))
			slow = true;

		f = fopen(argv[2], "r");
		if (!f) {
			perror("Error opening file");
			exit(5);
		}

		int len = fread(buf, 1, BUF_SIZE, f);
		if (len < BUF_SIZE) {
			fprintf(stderr, "Error reading file, must be %d bytes long\n", BUF_SIZE);
			exit(5);
		}

		u32 addr = 0;
		for (u32 i = 0; flash_blocks[i].count > 0; i++) {
			for (u32 j = 0; j < flash_blocks[i].count; j++) {
				flash_set_block_lock(dev, addr, false);
				flash_erase_block(dev, addr);
				flash_program_block(dev, addr, (void *) buf + addr, flash_blocks[i].size, slow);
				flash_set_block_lock(dev, addr, true);
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

		cx_read_mem(dev, flash_base, FLASH_SIZE, buf, MA_WORD);
		printf("done\n");
		if (fwrite(buf, 1, FLASH_SIZE, f) != FLASH_SIZE) {
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
