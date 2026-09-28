#ifndef MIPD_H
#define MIPD_H

#include <stdint.h>
#include <unistd.h>
#include <linux/if_packet.h>
#include <stdbool.h>
#include <stdint.h>
#include <linux/if_ether.h>

#define ETH_P_MIP 0x88B5
#define MIP_ADDR_BROADCAST 255u
#define MAX_INTERFACES 10 //would not expect more than 2 eth interfaces for node B, but having some headroom here could be nice.
#define MIP_HEADER_SIZE 4u //4 bytes = 32 bits

//MIP sdu types
#define MIP_SDU_TYPE_ARP 0x01
#define MIP_SDU_TYPE_PING 0x02

//SDU and PDU
#define MAX_SDU_BYTES (511u * 4)//SDU len header field has 9 bits, thus 2^9 = 512 possibilities. But we found 0 bit, so i think 511 should be set here
#define MAX_MIP_PDU_SIZE (MIP_HEADER_SIZE + MAX_SDU_BYTES)

//raw eth
#define ETHERNET_HEADER_SIZE ETH_HLEN //14 bytes
#define MAX_ETHERNET_FRAME_SIZE (ETHERNET_HEADER_SIZE + MAX_MIP_PDU_SIZE)

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

struct mip_header {
	uint8_t destination;
	uint8_t source;
	uint8_t ttl; //4 bit uint does not exist natively
	uint16_t sdu_length_words; //9 bits
	uint8_t sdu_type; //upper layer protocol type. 3 bits in spec
};

int encode_mip_header(const struct mip_header *header, uint8_t output[MIP_HEADER_SIZE]);

int decode_mip_header(const uint8_t input[MIP_HEADER_SIZE], struct mip_header *header);


#endif
