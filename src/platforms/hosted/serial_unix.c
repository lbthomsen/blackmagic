/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2019  Dave Marples <dave@marples.net>
 * Modifications (C) 2020 Uwe Bonnes (bon@elektron.ikp.physik.tu-darmstadt.de)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "general.h"
#include <sys/stat.h>
#include <sys/select.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <unistd.h>

#include "remote.h"
#include "bmp_hosted.h"
#include "cortexm.h"

static int fd; /* File descriptor for connection to GDB remote */

/* A nice routine grabbed from
 * https://stackoverflow.com/questions/6947413/how-to-open-read-and-write-from-serial-port-in-c
 */
static int set_interface_attribs(void)
{
	struct termios tty;
	memset(&tty, 0, sizeof tty);
	if (tcgetattr(fd, &tty) != 0) {
		DEBUG_WARN("error %d from tcgetattr", errno);
		return -1;
	}

	tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8; // 8-bit chars
	// disable IGNBRK for mismatched speed tests; otherwise receive break
	// as \000 chars
	tty.c_iflag &= ~IGNBRK; // disable break processing
	tty.c_lflag = 0;        // no signaling chars, no echo,
	// no canonical processing
	tty.c_oflag = 0;     // no remapping, no delays
	tty.c_cc[VMIN] = 0;  // read doesn't block
	tty.c_cc[VTIME] = 5; // 0.5 seconds read timeout

	tty.c_iflag &= ~(IXON | IXOFF | IXANY); // shut off xon/xoff ctrl

	tty.c_cflag |= (CLOCAL | CREAD); // ignore modem controls,
	// enable reading
	tty.c_cflag &= ~CSTOPB;
#if defined(CRTSCTS)
	tty.c_cflag &= ~CRTSCTS;
#endif
	if (tcsetattr(fd, TCSANOW, &tty) != 0) {
		DEBUG_WARN("error %d from tcsetattr", errno);
		return -1;
	}
	return 0;
}

#ifdef __APPLE__
int serial_open(BMP_CL_OPTIONS_t *cl_opts, char *serial)
{
	char name[4096];
	if (!cl_opts->opt_device) {
		/* Try to find some BMP if0*/
		if (!serial) {
			DEBUG_WARN("No serial device found\n");
			return -1;
		} else {
			sprintf(name, "/dev/cu.usbmodem%s1", serial);
		}
	} else {
		strncpy(name, cl_opts->opt_device, sizeof(name) - 1);
	}
	fd = open(name, O_RDWR | O_SYNC | O_NOCTTY);
	if (fd < 0) {
		DEBUG_WARN("Couldn't open serial port %s\n", name);
		return -1;
	}
	/* BMP only offers an USB-Serial connection with no real serial
     * line in between. No need for baudrate or parity.!
     */
	return set_interface_attribs();
}
#else
#define BMP_IDSTRING_BLACKSPHERE "usb-Black_Sphere_Technologies_Black_Magic_Probe"
#define BMP_IDSTRING_BLACKMAGIC  "usb-Black_Magic_Debug_Black_Magic_Probe"
#define BMP_IDSTRING_1BITSQUARED "usb-1BitSquared_Black_Magic_Probe"
#define DEVICE_BY_ID             "/dev/serial/by-id/"

static bool begins_with(const char *const str, const size_t str_length, const char *const value)
{
	const size_t value_length = strlen(value);
	if (str_length < value_length)
		return false;
	return memcmp(str, value, value_length) == 0;
}

static bool ends_with(const char *const str, const size_t str_length, const char *const value)
{
	const size_t value_length = strlen(value);
	if (str_length < value_length)
		return false;
	const size_t offset = str_length - value_length;
	return memcmp(str + offset, value, value_length) == 0;
}

bool device_is_bmp_gdb_port(const char *const name)
{
	const size_t length = strlen(name);
	if (begins_with(name, length, BMP_IDSTRING_BLACKSPHERE) || begins_with(name, length, BMP_IDSTRING_BLACKMAGIC) ||
		begins_with(name, length, BMP_IDSTRING_1BITSQUARED)) {
		return ends_with(name, length, "-if00");
	}
	return false;
}

int serial_open(BMP_CL_OPTIONS_t *cl_opts, char *serial)
{
	char name[4096];
	if (!cl_opts->opt_device) {
		/* Try to find some BMP if0*/
		DIR *dir = opendir(DEVICE_BY_ID);
		if (!dir) {
			DEBUG_WARN("No serial devices found\n");
			return -1;
		}
		size_t matches = 0;
		size_t total = 0;
		while (true) {
			const struct dirent *const entry = readdir(dir);
			if (entry == NULL)
				break;
			if (device_is_bmp_gdb_port(entry->d_name)) {
				++total;
				if (serial && strstr(entry->d_name, serial) == 0)
					continue;
				++matches;
				const size_t path_len = sizeof(DEVICE_BY_ID) - 1U;
				memcpy(name, DEVICE_BY_ID, path_len);
				const size_t name_len = strlen(entry->d_name);
				const size_t truncated_len = MIN(name_len, sizeof(name) - path_len - 2U);
				memcpy(name + path_len, entry->d_name, truncated_len);
				name[path_len + truncated_len] = '\0';
			}
		}
		closedir(dir);
		if (total == 0) {
			DEBUG_WARN("No Black Magic Probes found\n");
			return -1;
		}
		if (matches != 1) {
			DEBUG_INFO("Available Probes:\n");
			dir = opendir(DEVICE_BY_ID);
			if (dir) {
				while (true) {
					const struct dirent *const entry = readdir(dir);
					if (entry == NULL)
						break;
					if (device_is_bmp_gdb_port(entry->d_name))
						DEBUG_WARN("%s\n", entry->d_name);
				}
				closedir(dir);
				if (serial)
					DEBUG_WARN("No match for (partial) serial number \"%s\"\n", serial);
				else
					DEBUG_WARN("Select probe with `-s <(Partial) Serial Number>`\n");
			} else
				DEBUG_WARN("Could not scan %s: %s\n", name, strerror(errno));
			return -1;
		}
	} else {
		const size_t path_len = strlen(cl_opts->opt_device);
		const size_t truncated_len = MIN(path_len, sizeof(name) - 1U);
		memcpy(name, cl_opts->opt_device, truncated_len);
		name[truncated_len] = '\0';
	}
	fd = open(name, O_RDWR | O_SYNC | O_NOCTTY);
	if (fd < 0) {
		DEBUG_WARN("Couldn't open serial port %s\n", name);
		return -1;
	}
	/* BMP only offers an USB-Serial connection with no real serial
	 * line in between. No need for baudrate or parity.!
	 */
	return set_interface_attribs();
}
#endif

void serial_close(void)
{
	close(fd);
}

int platform_buffer_write(const uint8_t *data, int size)
{
	DEBUG_WIRE("%s\n", data);
	const int written = write(fd, data, size);
	if (written < 0) {
		DEBUG_WARN("Failed to write\n");
		exit(-2);
	}
	return size;
}

/* XXX: The size parameter should be size_t and we should either return size_t or bool */
/* XXX: This needs documenting that it can abort the program with exit(), or the error handling fixed */
int platform_buffer_read(uint8_t *data, int maxsize)
{
	char response = 0;
	struct timeval timeout = {
		.tv_sec = cortexm_wait_timeout / 1000,
		.tv_usec = 1000 * (cortexm_wait_timeout % 1000)
	};

	/* Drain the buffer for the remote till we see a start-of-response byte */
	while (response != REMOTE_RESP) {
		fd_set select_set;
		FD_ZERO(&select_set);
		FD_SET(fd, &select_set);

		const int result = select(FD_SETSIZE, &select_set, NULL, NULL, &timeout);
		if (result < 0) {
			DEBUG_WARN("Failed on select\n");
			return -3;
		}
		if (result == 0) {
			DEBUG_WARN("Timeout while waiting for BMP response\n");
			return -4;
		}
		if (read(fd, &response, 1) != 1) {
			DEBUG_WARN("Failed to read response\n");
			return -6;
		}
	}
	/* Now collect the response */
	for (size_t offset = 0; offset < (size_t)maxsize;) {
		fd_set select_set;
		FD_ZERO(&select_set);
		FD_SET(fd, &select_set);
		const int result = select(FD_SETSIZE, &select_set, NULL, NULL, &timeout);
		if (result < 0) {
			DEBUG_WARN("Failed on select\n");
			exit(-4);
		}
		if (result == 0) {
			DEBUG_WARN("Timeout on read\n");
			return -5;
		}
		if (read(fd, data + offset, 1) != 1) {
			DEBUG_WARN("Failed to read response\n");
			return -6;
		}
		if (data[offset] == REMOTE_EOM) {
			data[offset] = 0;
			DEBUG_WIRE("       %s\n", data);
			return offset;
		}
		++offset;
	}

	DEBUG_WARN("Failed to read\n");
	return -6;
}
