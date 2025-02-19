// This file is Copyright (c) 2013-2014 Sebastien Bourdeauducq <sb@m-labs.hk>
// This file is Copyright (c) 2014-2019 Florent Kermarrec <florent@enjoy-digital.fr>
// This file is Copyright (c) 2018 Ewen McNeill <ewen@naos.co.nz>
// This file is Copyright (c) 2018 Felix Held <felix-github@felixheld.de>
// This file is Copyright (c) 2019 Gabriel L. Somlo <gsomlo@gmail.com>
// This file is Copyright (c) 2017 Tim 'mithro' Ansell <mithro@mithis.com>
// This file is Copyright (c) 2018 William D. Jones <thor0505@comcast.net>
// License: BSD

#include <stdio.h>
#include <console.h>
#include <uart.h>
#include <system.h>
#include <crc.h>
#include <string.h>
#include <irq.h>

#include <generated/mem.h>
#include <generated/csr.h>

#ifdef CSR_ETHMAC_BASE
#include <net/microudp.h>
#include <net/tftp.h>
#endif

#include "sfl.h"
#include "boot.h"

extern void boot_helper(unsigned long r1, unsigned long r2, unsigned long r3, unsigned long addr);

static void __attribute__((noreturn)) boot(unsigned long r1, unsigned long r2, unsigned long r3, unsigned long addr)
{
	printf("Executing booted program at 0x%08x\n", addr);
	printf("--============= \e[1mLiftoff!\e[0m ===============--\n");
	uart_sync();
	irq_setmask(0);
	irq_setie(0);
/* FIXME: understand why flushing icache on Vexriscv make boot fail  */
#ifndef __vexriscv__
	flush_cpu_icache();
#endif
	flush_cpu_dcache();
#ifdef CONFIG_L2_SIZE
	flush_l2_cache();
#endif
	boot_helper(r1, r2, r3, addr);
	while(1);
}

enum {
	ACK_TIMEOUT,
	ACK_CANCELLED,
	ACK_OK
};

static int check_ack(void)
{
	int recognized;
	static const char str[SFL_MAGIC_LEN] = SFL_MAGIC_ACK;

	timer0_en_write(0);
	timer0_reload_write(0);
	timer0_load_write(CONFIG_CLOCK_FREQUENCY/4);
	timer0_en_write(1);
	timer0_update_value_write(1);
	recognized = 0;
	while(timer0_value_read()) {
		if(uart_read_nonblock()) {
			char c;
			c = uart_read();
			if((c == 'Q') || (c == '\e'))
				return ACK_CANCELLED;
			if(c == str[recognized]) {
				recognized++;
				if(recognized == SFL_MAGIC_LEN)
					return ACK_OK;
			} else {
				if(c == str[0])
					recognized = 1;
				else
					recognized = 0;
			}
		}
		timer0_update_value_write(1);
	}
	return ACK_TIMEOUT;
}

#define MAX_FAILED 5

/* Returns 1 if other boot methods should be tried */
int serialboot(void)
{
	struct sfl_frame frame;
	int failed;
	static const char str[SFL_MAGIC_LEN+1] = SFL_MAGIC_REQ;
	const char *c;
	int ack_status;

	printf("Booting from serial...\n");
	printf("Press Q or ESC to abort boot completely.\n");

	c = str;
	while(*c) {
		uart_write(*c);
		c++;
	}
	ack_status = check_ack();
	if(ack_status == ACK_TIMEOUT) {
		printf("Timeout\n");
		return 1;
	}
	if(ack_status == ACK_CANCELLED) {
		printf("Cancelled\n");
		return 0;
	}
	/* assume ACK_OK */

	failed = 0;
	while(1) {
		int i;
		int actualcrc;
		int goodcrc;

		/* Grab one frame */
		frame.length = uart_read();
		frame.crc[0] = uart_read();
		frame.crc[1] = uart_read();
		frame.cmd = uart_read();
		for(i=0;i<frame.length;i++)
			frame.payload[i] = uart_read();

		/* Check CRC */
		actualcrc = ((int)frame.crc[0] << 8)|(int)frame.crc[1];
		goodcrc = crc16(&frame.cmd, frame.length+1);
		if(actualcrc != goodcrc) {
			failed++;
			if(failed == MAX_FAILED) {
				printf("Too many consecutive errors, aborting");
				return 1;
			}
			uart_write(SFL_ACK_CRCERROR);
			continue;
		}

		/* CRC OK */
		switch(frame.cmd) {
			case SFL_CMD_ABORT:
				failed = 0;
				uart_write(SFL_ACK_SUCCESS);
				return 1;
			case SFL_CMD_LOAD: {
				char *writepointer;

				failed = 0;
				writepointer = (char *)(
					 ((unsigned long)frame.payload[0] << 24)
					|((unsigned long)frame.payload[1] << 16)
					|((unsigned long)frame.payload[2] <<  8)
					|((unsigned long)frame.payload[3] <<  0));
				for(i=4;i<frame.length;i++)
					*(writepointer++) = frame.payload[i];
				uart_write(SFL_ACK_SUCCESS);
				break;
			}
			case SFL_CMD_JUMP: {
				unsigned long addr;

				failed = 0;
				addr =   ((unsigned long)frame.payload[0] << 24)
					|((unsigned long)frame.payload[1] << 16)
					|((unsigned long)frame.payload[2] <<  8)
					|((unsigned long)frame.payload[3] <<  0);
				uart_write(SFL_ACK_SUCCESS);
				boot(0, 0, 0, addr);
				break;
			}
			default:
				failed++;
				if(failed == MAX_FAILED) {
					printf("Too many consecutive errors, aborting");
					return 1;
				}
				uart_write(SFL_ACK_UNKNOWN);
				break;
		}
	}
	return 1;
}

#ifdef CSR_ETHMAC_BASE

#ifndef LOCALIP1
#define LOCALIP1 192
#define LOCALIP2 168
#define LOCALIP3 1
#define LOCALIP4 50
#endif

#ifndef REMOTEIP1
#define REMOTEIP1 192
#define REMOTEIP2 168
#define REMOTEIP3 1
#define REMOTEIP4 100
#endif

#define DEFAULT_TFTP_SERVER_PORT 69  /* IANA well known port: UDP/69 */
#ifndef TFTP_SERVER_PORT
#define TFTP_SERVER_PORT DEFAULT_TFTP_SERVER_PORT
#endif

static int tftp_get_v(unsigned int ip, unsigned short server_port,
const char *filename, char *buffer)
{
	int r;

	r = tftp_get(ip, server_port, filename, buffer);
	if(r > 0)
		printf("Successfully downloaded %d bytes from %s over TFTP\n", r, filename);
	else
		printf("Unable to download %s over TFTP\n", filename);
	return r;
}

static const unsigned char macadr[6] = {0x10, 0xe2, 0xd5, 0x00, 0x00, 0x00};

void netboot(void)
{
	int size;
	unsigned int ip;
	unsigned long tftp_dst_addr;
	unsigned short tftp_port;

	printf("Booting from network...\n");
	printf("Local IP : %d.%d.%d.%d\n", LOCALIP1, LOCALIP2, LOCALIP3, LOCALIP4);
	printf("Remote IP: %d.%d.%d.%d\n", REMOTEIP1, REMOTEIP2, REMOTEIP3, REMOTEIP4);

	ip = IPTOINT(REMOTEIP1, REMOTEIP2, REMOTEIP3, REMOTEIP4);

	microudp_start(macadr, IPTOINT(LOCALIP1, LOCALIP2, LOCALIP3, LOCALIP4));

	tftp_port = TFTP_SERVER_PORT;
	printf("Fetching from: UDP/%d\n", tftp_port);

#ifdef NETBOOT_LINUX_VEXRISCV
	tftp_dst_addr = MAIN_RAM_BASE;
	size = tftp_get_v(ip, tftp_port, "Image", (void *)tftp_dst_addr);
	if (size <= 0) {
		printf("Network boot failed\n");
		return;
	}

	tftp_dst_addr = MAIN_RAM_BASE + 0x00800000;
	size = tftp_get_v(ip, tftp_port, "rootfs.cpio", (void *)tftp_dst_addr);
	if(size <= 0) {
		printf("No rootfs.cpio found\n");
		return;
	}

	tftp_dst_addr = MAIN_RAM_BASE + 0x01000000;
	size = tftp_get_v(ip, tftp_port, "rv32.dtb", (void *)tftp_dst_addr);
	if(size <= 0) {
		printf("No rv32.dtb found\n");
		return;
	}

	tftp_dst_addr = EMULATOR_RAM_BASE;
	size = tftp_get_v(ip, tftp_port, "emulator.bin", (void *)tftp_dst_addr);
	if(size <= 0) {
		printf("No emulator.bin found\n");
		return;
	}

	boot(0, 0, 0, EMULATOR_RAM_BASE);
#else
	tftp_dst_addr = MAIN_RAM_BASE;
	size = tftp_get_v(ip, tftp_port, "boot.bin", (void *)tftp_dst_addr);
	if (size <= 0) {
		printf("Network boot failed\n");
		return;
	}

	boot(0, 0, 0, MAIN_RAM_BASE);
#endif

}

#endif

#ifdef FLASHBOOT_LINUX_VEXRISCV

/* TODO: add configurable flash mapping, improve integration */

void flashboot(void)
{
	printf("Loading Image from flash...\n");
	memcpy((void *)MAIN_RAM_BASE + 0x00000000, (void *)0x50400000, 0x400000);

	printf("Loading rootfs.cpio from flash...\n");
	memcpy((void *)MAIN_RAM_BASE + 0x00800000, (void *)0x50800000, 0x700000);

	printf("Loading rv32.dtb from flash...\n");
	memcpy((void *)MAIN_RAM_BASE + 0x01000000, (void *)0x50f00000, 0x001000);

	printf("Loading emulator.bin from flash...\n");
	memcpy((void *)EMULATOR_RAM_BASE + 0x00000000, (void *)0x50f80000, 0x004000);

	boot(0, 0, 0, EMULATOR_RAM_BASE);
}
#else

#ifdef FLASH_BOOT_ADDRESS

/* On systems with exernal SDRAM we copy out of the SPI flash into the SDRAM
   before running, as it is faster.  If we have no SDRAM then we have to
   execute directly out of the SPI flash. */
#ifdef MAIN_RAM_BASE
#define FIRMWARE_BASE_ADDRESS MAIN_RAM_BASE
#else
/* Firmware code starts after (a) length and (b) CRC -- both unsigned ints */
#define FIRMWARE_BASE_ADDRESS (FLASH_BOOT_ADDRESS + 2 * sizeof(unsigned int))
#endif

void flashboot(void)
{
	unsigned int *flashbase;
	unsigned int length;
	unsigned int crc;
	unsigned int got_crc;

	printf("Booting from flash...\n");
	flashbase = (unsigned int *)FLASH_BOOT_ADDRESS;
	length = *flashbase++;
	crc = *flashbase++;
	if((length < 32) || (length > 4*1024*1024)) {
		printf("Error: Invalid flash boot image length 0x%08x\n", length);
		return;
	}

#ifdef MAIN_RAM_BASE
	printf("Loading %d bytes from flash...\n", length);
	memcpy((void *)MAIN_RAM_BASE, flashbase, length);
#endif

	got_crc = crc32((unsigned char *)FIRMWARE_BASE_ADDRESS, length);
	if(crc != got_crc) {
		printf("CRC failed (expected %08x, got %08x)\n", crc, got_crc);
		return;
	}
	boot(0, 0, 0, FIRMWARE_BASE_ADDRESS);
}
#endif

#endif

#ifdef ROM_BOOT_ADDRESS
/* When firmware is small enough, it can be interesting to run code from an
   embedded blockram memory (faster and not impacted by memory controller
   activity). Define ROM_BOOT_ADDRESS for that and initialize the blockram
   with the firmware data. */
void romboot(void)
{
	boot(0, 0, 0, ROM_BOOT_ADDRESS);
}
#endif
