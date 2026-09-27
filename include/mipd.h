#ifndef MIPD_H
#define MIPD_H

#include <stdint.h>
#include <unistd.h>
#include <linux/if_packet.h>
#include <stdbool.h>
#include <stdint.h>

#define ETH_P_MIP 0x88B5
#define MIP_ADDR_BROADCAST 255u
#define MAX_INTERFACES 10 //would not expect more than 2 eth interfaces for node B, but having some headroom here could be nice.

struct daemon_context {
	bool debug;
	uint8_t mip_address; //8 bit int, src/dst for passing up/down

	int raw_socket;//network connection
				//
	int upper_listening_file_descriptor; //listen for new connection if old one is terminated.
	int upper_client_file_descriptor;//local client
					 //
	const char *socket_upper_path;

	struct sockaddr_ll interfaces[MAX_INTERFACES];
	unsigned int interface_count;
};

void print_usage(const char *program_name);

//struct mip_header {}


#endif
