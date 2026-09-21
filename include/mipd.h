#ifndef MIPD_H
#define MIPD_H

#include <stdint.h>
#include <unistd.h>
#include <linux/if_packet.h>
#include <stdbool.h>
#include <stdint.h>

struct daemon_context {
	bool debug;
	uint8_t mip_address; //8 bit int, src/dst for passing up/down

	int raw_file_descriptor;
	int upper_listen_file_descriptor;
	int upper_client_file_descriptor;
};

void print_usage(const char *program_name);

//struct mip_header {}


#endif
