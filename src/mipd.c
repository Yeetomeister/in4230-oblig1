/*
 * No global variables are used in this daemon. All states are kept in the struct daemon_context and passed to functions as needed.
 */


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

//Includes for initial unix socket attempt.
#include <sys/epoll.h> //use epoll as we want a queue of incoming requests
#include <sys/socket.h>
#include <sys/un.h>

//raw sockets
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <sys/socket.h>

//eth
#include <ifaddrs.h>

#include "../include/mipd.h"


/*
 *print how to run the program
 *
 *program_name: argv[0]
 *
 *does not return any value, only prints.
 */
void print_usage(const char *program_name){
	printf("usage: %s [-h] [-d] <socket_upper> <MIP address>\n", program_name);
}


/*
 *converts text into a mip address
 *
 *text: the string from the command line
 *result: receives the mip address given that it was valid
 *
 *returns 0 on success or -1 on error.
 */
static int parse_mip_address(const char *text, uint8_t *result){
	char *end;
	unsigned long value;

	//string to unsigned long int
	value = strtoul(text, &end, 10);

	if(text[0] == '\0' || *end != '\0' || value > 254){
		return -1;
	}

	*result = (uint8_t)value;
	return 0;
}

/**
 *Create a file descriptor that listens for connections from client.
 *Perform some error checking before returning a hopefully valid FD.
 *
 *socket_path: file path for which file we want a unix socket on. 
 *
 *return file descriptor for created socket, or -1 on error.
 *
 *Inspired by chat.c used in plnary 09.02.26
 *https://github.com/kristjoc/plenaries-in3230-in4230-h26/blob/main/p2_02-09-2026/sockets/unix_sockets/chat.c
 */
static int create_upper_listening_socket(const char *socket_path){
	struct sockaddr_un address;
	int file_descriptor;

	file_descriptor = socket(AF_UNIX, SOCK_SEQPACKET, 0);

	if (file_descriptor == -1) {
		perror("failed to get socket from file dscrptr");
		return -1;
	}

	/**
	 * Clearing size of whole structre to ensure enough address space is allocated
	 */
	memset(&address, 0, sizeof(struct sockaddr_un));
	address.sun_family = AF_UNIX;

	if (strlen(socket_path) >= sizeof(address.sun_path)){
		fprintf(stderr, "Unix socket path is too long \n");
		close(file_descriptor);
		return -1;
	}

	/**
	 *Character arrays contain null terminator, so we subtract 1 when calculating the required size for the supplied unix path.
	 */
	strncpy(address.sun_path, socket_path, sizeof(address.sun_path) -1);

	/**
	 * Unlink socket in case file-system entry is still present
	 */
	unlink(socket_path);

	if (bind(file_descriptor, (struct sockaddr *)&address, sizeof(address)) == -1) {
		perror("bind");
		close(file_descriptor);
		return -1;
	}

	/**
	 *Specify 1 for backlog so that 1 incoming conneciton request can wait
	 */
	if (listen(file_descriptor, 1) == -1) {
		perror("listen");
		close(file_descriptor);
		unlink(socket_path);
		return -1;
	}

	return file_descriptor;

}

/**
 *Accept upper-layer application to connect to mipd through UNIX file descriptor provided
 *
 * Returns file descriptor of connected client or -1 on error.
 * Inspired by line 188: https://github.com/kristjoc/plenaries-in3230-in4230-h26/blob/main/p2_02-09-2026/sockets/unix_sockets/chat.c 
 */
static int accept_upper_client(int listening_file_descriptor) {
	int client_file_descriptor;

	client_file_descriptor = accept(listening_file_descriptor, NULL, NULL);

	if (client_file_descriptor == -1) {
		perror("failed to accept listening_file_descriptor\n");
		return -1;
	}

	printf("local client connected\n");

	return client_file_descriptor;

}

//because c only knows functions that are prior we have to create a promise that this function exist prior to receive_upper_layer_message
static int send_or_queue(struct daemon_context *context, uint8_t destination, const uint8_t *sdu, size_t sdu_length);
/**
 *Receive one message from connected upper layer appliction
 * message format from specification [8bit MIP address][payload (sdu)]
 *
 * context: daemon state where we read the upper_client_file_descriptor
 *
 *returns 1 on message receive, 0 on client disconnect and -1 on error or incomplete message(only received address)
 */
static int receive_upper_layer_message(struct daemon_context *context) {
	uint8_t buffer[1024];
	ssize_t received_bytes;

	received_bytes = recv(context->upper_client_file_descriptor, buffer, sizeof(buffer), 0);

	if(received_bytes == -1) {
		perror("invalid receive");
		return -1;
	}

	//0 denotes that connection was closed, so this return value is useful
	if (received_bytes == 0) {
		//printf("Client closed connection without sending data\n");
		return 0;
	}

	//only address byte without payload
	if (received_bytes == 1) {
		printf("Received address but no message.\n");
		return -1;
	}

	if(context->debug) {
		printf("[upper] message for MIP %u: ", (unsigned int)buffer[0]);
		fwrite(buffer + 1, 1, (size_t)received_bytes - 1, stdout);
		printf("\n");
	}

	//buffer[0] is dst address. Rest is sdu
	if(send_or_queue(context, buffer[0], buffer + 1, (size_t)received_bytes -1) == -1) {
		return -1;
	}

	//return 1 to denote success
	return 1;
}

/**
 *create the raw AF_PACKET socket that sends and receives ethernet frames with ethertype ETH_P_MIP
 *only mip frames are delivered to it because of the protocol argument
 *
 *returns socket file descriptor or -1 on error.
 *
 *Basically copy paste of line 180
https://github.com/kristjoc/plenaries-in3230-in4230-h26/blob/main/p2_02-09-2026/sockets/raw_sockets/sender.c
 */
static int create_mip_raw_socket(void) {
	int raw_sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_MIP));

	if (raw_sock == -1) {
		perror("Raw socket fail\n");
		return -1;
	}

	return raw_sock;
}

/**
 *Find packet interfaces
 *
 *context: daemon state that  will receive the discovered interfaces to their sockaddr_ll interface list.
 *
 * Return 0 if one or multiple interfaces was found, -1 on error.
 */
static int discover_interfaces(struct daemon_context *context) {
	struct ifaddrs *interfaces;
	struct ifaddrs *current;

	if (getifaddrs(&interfaces) == -1) {
		perror("failed to get eth address");
		return -1;
	}

	//arrow because we have pointer to the context
	context->interface_count = 0;

	for (current = interfaces; current != NULL; current = current->ifa_next) {
		if(current->ifa_addr == NULL) {
			continue;
		}
		if (current->ifa_addr->sa_family != AF_PACKET) {
			continue;
		}
		if(strcmp(current->ifa_name, "lo") == 0) {
			continue;
		}
		if (context->interface_count >= MAX_INTERFACES) {
			fprintf(stderr, "Too many network interfaces\n");
			freeifaddrs(interfaces);
			return -1;
		}

		//Place the current ifa address into context interface list at index corresponding to the amunt of existing addresses
		memcpy(&context->interfaces[context->interface_count],current->ifa_addr, sizeof(struct sockaddr_ll));


		context->interface_count++;

	}

	freeifaddrs(interfaces);

	if(context->interface_count == 0) {
		fprintf(stderr,"no usable ethernet interfaces found.\n");
		return -1;
	}
	
	return 0;

}

/*
 *print a mac address as hex bytes seperated by ":"
 *
 *mac: pointer to the address bytes
 *length: the number of bytes, which should be 6
 *
 * no return, only print
 */
static void print_mac_address(const unsigned char *mac, unsigned char length) {
	unsigned int i;

	for (i = 0; i < length; i++) {
		printf("%02x%s",mac[i], (i + 1 == length) ? "" : ":");
	}
}

/**
 *pack MIP header fields into a four-byte
 *
 *header: fields t encode
 *output: four-byte buffer taht receives the encoded header
 *
 * return 0 on success, -1 on error
 */
int encode_mip_header(const struct mip_header *header, uint8_t output[MIP_HEADER_SIZE]) {
	uint32_t packed;
	uint32_t network_order;

	if (header == NULL || output == NULL) {
		fprintf(stderr, "provided argument is NULL\n");
		return -1;
	}

	//from the mip spec we find that the header should be 32 bit. We can fit all of it in a uint32_t
	packed = ((uint32_t)header->destination << 24)
		| ((uint32_t)header->source << 16)
		| ((uint32_t)header->ttl << 12)
		| ((uint32_t)header->sdu_length_words << 3)
		| (uint32_t)header->sdu_type;

	network_order = htonl(packed);
	memcpy(output, &network_order, MIP_HEADER_SIZE);

	return 0;
		
}

/**
 *Unpack four network bytes into mip header fields
 *
 *input: four byte encoded header
 *header: struct that receives the output fields
 *
 *return 0 on success or -1 on error
 */
int decode_mip_header(const uint8_t input[MIP_HEADER_SIZE], struct mip_header *header) {
	uint32_t network_order;
	uint32_t packed;

	if (input == NULL || header == NULL) {
		return -1;
	}

	memcpy(&network_order, input, MIP_HEADER_SIZE);
	packed = ntohl(network_order);


	header->destination = (uint8_t)(packed >> 24);
	header->source = (uint8_t)(packed >> 16);

	//ttl:4b, uint8:8b, mask: 0000 1111 = F
	header->ttl = (uint8_t)((packed >> 12) & 0x0F);

	//sdu_len:9b, uint16:16b. mask: 0000 0001 1111 1111 = 1FF
	header->sdu_length_words = (uint16_t)((packed >> 3) & 0x01FF);

	//sdu_type:3b, uint8:8b, mask:0000 0111 = 7
	header->sdu_type = (uint8_t)(packed & 0x07);

	return 0;
}

/**
 *Build mip pdu from provided header and sdu payload.
 *
 *header: header consisting of mip addresses, ttl and sdu type
 *sdu: payload bytes
 *sdu_length_bytes: number of data payload bytes
 *pdu: output buffer for encoded header and padded sdu.
 *pdu_capacity: number of available bytes in the pdu.
 *pdu_length: receives number of bytes written to the pdu.
 *
 *returns 0 on success or -1 on error. Error on invalid arguments, too large SDU, too small output buffer, header failes to encoded.
 *sdu is compied and appended zero bytes as padding to achieve multiple of four.
 */
static int build_mip_pdu(const struct mip_header *header,
			const uint8_t *sdu,
			size_t sdu_length_bytes,
			uint8_t *pdu,
			size_t pdu_capacity,
			size_t *pdu_length) {

	struct mip_header completed_header;
	size_t padded_sdu_length;

	if(header == NULL || pdu == NULL || pdu_length == NULL) {
		return -1;
	}

	if(sdu_length_bytes > 0 && sdu == NULL) {
		return -1;
	}

	if(sdu_length_bytes > MAX_SDU_BYTES) {
		return -1;
	}

	//from mip spec, round to next multiple of 4. Use integer div to discard decimals then multiply up to a whole multiple of 4
	padded_sdu_length = ((sdu_length_bytes + 3u) / 4u) * 4u;

	if (MIP_HEADER_SIZE + padded_sdu_length > pdu_capacity) {
		return -1;
	}

	completed_header = *header;
	completed_header.sdu_length_words = (uint16_t)(padded_sdu_length / 4u);

	if (encode_mip_header(&completed_header, pdu) == -1) {
		return -1;
	}

	if(sdu_length_bytes > 0) {
		memcpy(pdu + MIP_HEADER_SIZE, sdu, sdu_length_bytes);
	}

	memset(pdu + MIP_HEADER_SIZE + sdu_length_bytes, 0, padded_sdu_length - sdu_length_bytes);

	*pdu_length = MIP_HEADER_SIZE + padded_sdu_length;

	return 0;
}


/**
 *put a MIP PDU into an ethernet frame. dst mac(6) + src mac(6) + ethertype 0x88b5(2) + PDU
 *
 *source_mac, destination_mac: 6 byte mac address
 *pdu, pdu_length: the MIP pdu from build_mip_pdu
 *frame: output buffer
 *frame_capacity: output buffer size in bytes
 *frame_length: receives the total number of bytes written
 *
 *returns 0 on success, -1 on null arguments or if frame does not fit in the given buffer
 */
static int build_ethernet_frame(const uint8_t destination_mac[6],
				const uint8_t source_mac[6],
				const uint8_t *pdu,
				size_t pdu_length,
				uint8_t *frame,
				size_t frame_capacity,
				size_t *frame_length) {
	uint16_t network_ethertype;

	if(destination_mac == NULL || source_mac == NULL || pdu == NULL || frame == NULL || frame_length ==  NULL) {
		return -1;
	}

	if (ETHERNET_HEADER_SIZE + pdu_length > frame_capacity) {
		return -1;
	}

	memcpy(frame, destination_mac, 6);
	memcpy(frame + 6, source_mac, 6);

	network_ethertype = htons(ETH_P_MIP);
	memcpy(frame + 12, &network_ethertype, sizeof(network_ethertype));

	memcpy(frame + ETHERNET_HEADER_SIZE, pdu, pdu_length);

	*frame_length = ETHERNET_HEADER_SIZE + pdu_length;

	return 0;
}

/*
 *send a finished ethernet frame out of an interface with sendto() over a raw socket
 *
 *contex: daemon state. Raw socket and interface list
 *interface_number: the index into context->interfaces that chooses outgoing interface.
 *destination_mac: a 6 byte MAC put into sockaddr_ll for the kernel.
 *frame, frame_length: the complete frame built by build_ethernet_frame
 *
 *returns 0 on success, -1 on NULL arguments, invalid interface number or error by sendto
 */
static int send_ethernet_frame(struct daemon_context *context,
				unsigned int interface_number,
				const uint8_t destination_mac[6],
				const uint8_t *frame,
				size_t frame_length) {

	struct sockaddr_ll target;
	ssize_t sent_bytes;

	if (context == NULL || destination_mac == NULL || frame == NULL) {
		return -1;
	}

	if (interface_number >= context->interface_count) {
		fprintf(stderr, "invalid interface number");
		return -1;
	}

	memset(&target, 0, sizeof(target));
	target.sll_family = AF_PACKET;
	target.sll_protocol = htons(ETH_P_MIP);
	target.sll_ifindex = context->interfaces[interface_number].sll_ifindex;
	target.sll_halen = 6;
	memcpy(target.sll_addr, destination_mac, 6);

	sent_bytes = sendto(context->raw_socket, frame, frame_length, 0, (struct sockaddr *)&target, sizeof(target));

	if (sent_bytes == -1) {
		perror("failed to send bytes");
		return -1;
	}

	return 0;
}


/**
 *register file descriptor in the epoll instance so that epoll_wait reports when its ready
 *inspired by add_to_epoll_table in chat.c plenary 02.09.
 *
 *epoll_file_descriptor: the epoll instance from epoll_create1
 *file_desriptor: socket that we want to watch
 *
 *return 0 on success, -1 on error
 */
static int add_to_epoll(int epoll_file_descriptor, int file_descriptor) {
	struct epoll_event event;

	memset(&event, 0, sizeof(event));
	event.events = EPOLLIN; //will notify when there is something to be read.
	event.data.fd = file_descriptor; //keep track of the socket that notified
					 //
	if (epoll_ctl(epoll_file_descriptor, EPOLL_CTL_ADD, file_descriptor, &event) == -1) {
		perror("epoll_ctl failed");
		return -1;
	}

	return 0;
}


/**
 *find which of the interfaces in context has a given kernel interface index
 *
 * context: daemon state with a list of interfaces that we wish to search
 * interface_index: sll_ifindex as reported by kernel from recvfrom
 *
 *return index of requested interface or -1 if not found
 */
static int find_interface_by_index(const struct daemon_context *context, int interface_index) {
	unsigned int i;

	for(i = 0; i < context->interface_count; i++) {
		if(context->interfaces[i].sll_ifindex == interface_index) {
			return (int)i;
		}
	}

	return -1;
}

/**
 *print all valid entry in the mip-arp cache
 *
 *context: daemon state holding the table we inquire about
 *
 *no return, simply prints the result
 */
static void print_arp_cache(const struct daemon_context *context) {
	unsigned int address;
	bool empty = true;

	printf("MIP-ARP cache:\n");

	for (address = 0; address < ARP_CACHE_SIZE; address++) {
		const struct arp_entry *entry = &context->arp_cache[address];

		if(!entry->valid) {
			continue;
		}

		empty = false;
		printf(" MIP %3u -> ", address);
		print_mac_address(entry->mac_address, MAC_ADDRESS_LENGTH);
		printf(" (interface index %d)\n", context->interfaces[entry->interface_number].sll_ifindex);
	}

	if (empty) {
		printf(" arp cache empty\n");
	}
}



/**
 *store or overwrite mapping of a mip address in arp cache
 *
 *context: daemon state holding the cache
 *mip_address: the address for which entry we want to add or modify
 *mac_address: the mac address which we want to add or modify
 *interface_number: the interface that the neightbour is reached through
 *
 */
static void arp_cache_update(struct daemon_context *context,
				uint8_t mip_address,
				const uint8_t mac_address[MAC_ADDRESS_LENGTH],
				unsigned int interface_number) {
	
	struct arp_entry *entry = &context->arp_cache[mip_address];

	entry->valid = true;
	memcpy(entry->mac_address, mac_address, MAC_ADDRESS_LENGTH);
	entry->interface_number = interface_number;
}


/*
 *build a pdu inside an ether frame and send it out a specified interface
 *
 *context: daemon state
 *interface_number: which interface to send to
 *destination_mac: ethernet destination
 *destination_mip: mip destination address
 *ttl: mip time to live before packet is discarded
 *sdu_type: ARP or PING
 *sdu: payload
 *sdu_length: length of payload
 *
 *return 0 on success, -1 if packet building or sending fails
 * with debug mode this will print addresses and arp cache
 */
static int send_mip_packet(struct daemon_context *context,
			unsigned int interface_number,
			const uint8_t destination_mac[MAC_ADDRESS_LENGTH],
			uint8_t destination_mip,
			uint8_t ttl,
			uint8_t sdu_type,
			const uint8_t *sdu,
			size_t sdu_length) {
	
	struct mip_header header;
	uint8_t pdu[MAX_MIP_PDU_SIZE];
	uint8_t frame[MAX_ETHERNET_FRAME_SIZE];
	size_t pdu_length;
	size_t frame_length;
	const uint8_t *source_mac = context->interfaces[interface_number].sll_addr;

	header.destination = destination_mip;
	header.source = context->mip_address;
	header.ttl = ttl;
	header.sdu_length_words = 0;
	header.sdu_type = sdu_type;

	if (build_mip_pdu(&header, sdu, sdu_length, pdu, sizeof(pdu), &pdu_length) == -1){
		fprintf(stderr, "failed to build MIP pdu\n");
		return -1;
	}

	if(build_ethernet_frame(destination_mac, source_mac, pdu, pdu_length, frame, sizeof(frame), &frame_length) == -1) {
		fprintf(stderr, "failed to build ethernet frame\n");
		return -1;	
	}

	if(send_ethernet_frame(context, interface_number, destination_mac, frame, frame_length) == -1) {
		return -1;
	}

	if (context->debug) {
		printf("[send] %s MAC ", sdu_type == MIP_SDU_TYPE_ARP ? "MIP_ARP" : "PING");
		print_mac_address(source_mac, MAC_ADDRESS_LENGTH);
		printf(" -> ");
		print_mac_address(destination_mac, MAC_ADDRESS_LENGTH);
		printf(" MIP %u -> %u\n", context->mip_address, destination_mip);
		print_arp_cache(context);
	}

	return 0;
}


/**
 *Pack a mip-arp message into 4 byte sdu (type 1 bit, address 8 bits, 23 padd)
 *
 *type: MIP_ARP_REQUEST or MIP_ARP_RESPONSE
 *address: the mip address requested / answered
 *output: 4 byte buffer receiving the SDU put in network byte order by utonl
 *
 *no return
 */
static void encode_arp_sdu(uint8_t type, uint8_t address, uint8_t output[MIP_ARP_SDU_SIZE]) {
	uint32_t packed;

	packed = ((uint32_t)(type & 0x01) << 31) | ((uint32_t)address << 23);
	packed = htonl(packed);
	memcpy(output, &packed, MIP_ARP_SDU_SIZE);
}


/**
 *unpack 4 byte mip-arp sdu
 *
 *input: the 4 sdu bytes in network byte order
 *type, address: receive decoded fields
 *
 * no return
 */
static void decode_arp_sdu(const uint8_t input[MIP_ARP_SDU_SIZE], uint8_t *type, uint8_t *address) {
	uint32_t packed;

	memcpy(&packed, input, MIP_ARP_SDU_SIZE);
	packed = ntohl(packed);

	*type = (uint8_t)((packed >> 31) & 0x01);
	*address = (uint8_t)((packed >> 23) & 0xFF);
}


/*
 *broadcast a mip-arp request on every interface
 *
 *context: daemon state
 *lookup_address: MIP that we want the MAC for
 *
 *returns 0 if arp was sent on a least one interface, otherwise retunr -1
 */
static int send_arp_request(struct daemon_context *context, uint8_t lookup_address) {
	const uint8_t broadcast_mac[MAC_ADDRESS_LENGTH] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	uint8_t sdu[MIP_ARP_SDU_SIZE];
	unsigned int i;
	int sent = 0;

	encode_arp_sdu(MIP_ARP_REQUEST, lookup_address, sdu);

	for (i = 0; i < context->interface_count; i++) {
		if(send_mip_packet(context, i, broadcast_mac, MIP_ADDR_BROADCAST, MIP_TTL_BROADCAST, MIP_SDU_TYPE_ARP, sdu, sizeof(sdu)) == 0){
			sent++;
		}
	}

	if (sent > 0) {
		return 0;
	}
	else {
		return -1;
	}
}


/**
 *send sdu from upper layer to a destination or park the sdu and send arp first
 *
 *context: daemon state for arp cache and pending slot
 *destination: mip address from the first byte of the upper layer messsage. indicating dest
 *sdu, sdu_length: remaining message which is payload and payload length
 *
 *return 0 if packet was sent or queued. -1 on error for too long message or send error.
 */
static int send_or_queue(struct daemon_context *context, uint8_t destination, const uint8_t *sdu, size_t sdu_length) {
	struct arp_entry *entry = &context->arp_cache[destination];

	if (sdu_length > MAX_SDU_BYTES){
		fprintf(stderr, "message too long for mip sdu");
		return -1;
	}

	//if cache hit for mac and interface we can send imediately
	if (entry->valid) {
		return send_mip_packet(context, entry->interface_number, entry->mac_address, destination, MIP_TTL_DEFAULT, MIP_SDU_TYPE_PING, sdu, sdu_length);
	}

	//cache miss. We keep the packet and send arp request.
	context->pending.active = true;
	context->pending.destination = destination;
	memcpy(context->pending.sdu, sdu, sdu_length);
	context->pending.sdu_length = sdu_length;

	return send_arp_request(context, destination);
}

/*
 *pass a received sdu up to connected app in format [source mip][sdu]
 *
 *context: daemon state with upper client socket
 *source: mmip address of the sender
 *sdu, sdu_length: payload of redeived PDU
 *
 *return 0 on success, -1 if no app is connected or sending fails
 *
 */
static int send_to_upper_layer(struct daemon_context *context, uint8_t source, const uint8_t *sdu, size_t sdu_length) {
	uint8_t buffer[1 + MAX_SDU_BYTES];

	if (context->upper_client_file_descriptor == -1) {
		printf("no local app connected, dropping packet from MIP %u \n", source);
		return -1;
	}

	buffer[0] = source;
	memcpy(buffer + 1, sdu, sdu_length);

	if(send(context->upper_client_file_descriptor, buffer, sdu_length + 1, 0) == -1) {
		perror("send to upper layer failed");
		return -1;
	}
	
	return 0;
}

/*
 *handle mip-atp sdu like specified in 6.2
 *request for our address: learn sender and answer the interface it arrived on
 *response: learn sender and send the pending packet if it was waiting for this address
 *
 *context: daemon state
 *header: decoded MIP header of the received PDU.
 *source_mac: ethernet source of the frame
 *interface_number: which of our interfaces taht the frame arrive on
 *sdu, sdu_length the arp sdu itself
 *
 * retunrs 0 on successful handling(regardless if packet was for us of not) and -1 on error.
 */
static int handle_arp(struct daemon_context *context,
			const struct mip_header *header,
			const uint8_t source_mac[MAC_ADDRESS_LENGTH],
			unsigned int interface_number,
			const uint8_t *sdu,
			size_t sdu_length) {
	
	uint8_t type;
	uint8_t address;
	uint8_t response[MIP_ARP_SDU_SIZE];

	//catch invalid arp sdu
	if (MIP_ARP_SDU_SIZE > sdu_length) {
		return -1;
	}

	decode_arp_sdu(sdu, &type, &address);

	if (type == MIP_ARP_REQUEST) {
		//request is not for us, but we handled correctly and therefore return 0
		if(address != context->mip_address) {
			return 0;
		}

		arp_cache_update(context, header->source, source_mac, interface_number);

		encode_arp_sdu(MIP_ARP_RESPONSE, context->mip_address, response);

		return send_mip_packet(context, interface_number, source_mac, header->source, MIP_TTL_BROADCAST, MIP_SDU_TYPE_ARP, response, sizeof(response));
	}

	//arp response
	arp_cache_update(context, header->source, source_mac, interface_number);

	if (context->pending.active && context->pending.destination == header->source) {
		context->pending.active = false;
		return send_or_queue(context, context->pending.destination, context->pending.sdu, context->pending.sdu_length);
	}

	return 0;
}

/*
 *read one frame from the raw socket and parse + dispatch it on sdu type
 *
 *context: daemon state
 *
 *returns 0 when frame was handled or ignored, -1 on error.
 *frames that are too short, have bad length field or are not addressed to us are dropped
 */
static int handle_raw_frame(struct daemon_context *context) {
	uint8_t frame[MAX_ETHERNET_FRAME_SIZE];
	struct sockaddr_ll from;
	socklen_t from_length = sizeof(from);
	ssize_t frame_length;
	struct mip_header header;
	const uint8_t *destination_mac;
	const uint8_t *source_mac;
	const uint8_t *sdu;
	size_t sdu_length;
	int interface_number;

	frame_length = recvfrom(context->raw_socket, frame, sizeof(frame), 0, (struct sockaddr *)&from, &from_length);

	if(frame_length == -1) {
		perror("recv from failed for raw");
		return -1;
	}

	if((size_t)frame_length < ETHERNET_HEADER_SIZE + MIP_HEADER_SIZE) {
		return 0;
	}

	interface_number = find_interface_by_index(context, from.sll_ifindex);

	if (interface_number == -1) {
		return 0;
	}

	destination_mac = frame;
	source_mac = frame + MAC_ADDRESS_LENGTH;

	decode_mip_header(frame + ETHERNET_HEADER_SIZE, &header);

	sdu = frame + ETHERNET_HEADER_SIZE + MIP_HEADER_SIZE;
	sdu_length = (size_t)header.sdu_length_words * 4;

	if (ETHERNET_HEADER_SIZE + MIP_HEADER_SIZE + sdu_length > (size_t)frame_length) {
		fprintf(stderr, "SDU length field is lnger than frame. dropping\n");
		return 0;
	}

	//only for us or broadcast
	if (header.destination != context->mip_address && header.destination != MIP_ADDR_BROADCAST) {
		return 0;
	}

	if (context->debug) {
		printf("[recv] %s MAC ", header.sdu_type == MIP_SDU_TYPE_ARP ? "MIP_ARP" : "PING");
		print_mac_address(source_mac, MAC_ADDRESS_LENGTH);
		printf(" -> ");
		print_mac_address(destination_mac, MAC_ADDRESS_LENGTH);
		printf(" MIP %u -> %u\n", header.source, header.destination);
	}

	switch (header.sdu_type) {
		case MIP_SDU_TYPE_ARP:
			handle_arp(context, &header, source_mac, (unsigned int)interface_number, sdu, sdu_length);
			break;
		case MIP_SDU_TYPE_PING:
			send_to_upper_layer(context, header.source, sdu, sdu_length);
			break;
		default:
			printf("unknown sdu type %u, dropping\n", header.sdu_type);
			break;
	}

	if (context->debug) {
		print_arp_cache(context);
	}

	return 0;
}

/*
 *entry point of the mip daemon. Parses input arguments, set up unix socket, raw socket and interfaces. Then runs epoll event loop forever.
 *
 *return 0 on normal exit, 1 on bad arguments or if socket could not be set up
 */
int main(int argc, char *argv[]){
	struct daemon_context context = {
		.debug = false,
		.mip_address = 0,
		.raw_socket = -1,
		.upper_listening_file_descriptor = -1,
		.upper_client_file_descriptor = -1,
		.socket_upper_path = NULL
	};


	const char *socket_path;
	const char *address_text;

	//loop variables
	int epoll_file_descriptor;		//epoll instance watching the sockets
	struct epoll_event events[MAX_EVENTS]; 	//is filled by epoll_wait with ready sockets
	int ready_count;			//num of ready sockets. returned by epoll_wait
	int i;

	setvbuf(stdout, NULL, _IOLBF, 0); //write out everything for testing with mininet running

	if (argc == 2 && strcmp(argv[1], "-h") == 0) {
		print_usage(argv[0]);
		return 0;
	}
	else if (argc == 4 && strcmp(argv[1], "-d") == 0) {
		context.debug = true;
		socket_path = argv[2];
		address_text = argv[3];

		//start loop

		//return 0;
	}

	else if (argc == 3){
		socket_path = argv[1];
		address_text = argv[2];

		//start loop

		//return 0;
	}
	else{
		print_usage(argv[0]);
		return 1;
	}

	if (parse_mip_address(address_text, &context.mip_address) == -1){
		printf("MIP address has to be number between 0 and 254\n");
		return 1;
	}

	context.socket_upper_path = socket_path;

	//Create UNIX listening socket
	context.upper_listening_file_descriptor = create_upper_listening_socket(context.socket_upper_path);

	if (context.upper_listening_file_descriptor == -1) {
		return 1;
	}

	//Create unix raw socket
	context.raw_socket = create_mip_raw_socket();

	if (context.raw_socket == -1) {
		close(context.upper_listening_file_descriptor);
		unlink(context.socket_upper_path);
		return 1;
	}
	printf("raw mip socket was created\n");
	
	
	if(discover_interfaces(&context) == -1) {
		close(context.raw_socket);
		close(context.upper_listening_file_descriptor);
		unlink(context.socket_upper_path);
		return 1;
	}
	printf("Found %u network interfaces\n", context.interface_count);

	if (context.debug) {
		unsigned int i;

		for (i = 0; i < context.interface_count; i++) {
			const struct sockaddr_ll *interface = &context.interfaces[i];
			printf("Interface index %d, mac ", interface->sll_ifindex);
			print_mac_address(interface->sll_addr, interface->sll_halen);
			printf("\n");
		}
	}

	//context.upper_client_file_descriptor = accept_upper_client(context.upper_listening_file_descriptor);
	epoll_file_descriptor = epoll_create1(0);


	if(epoll_file_descriptor == -1) {
		perror("epoll_create1 fail");
		close(context.raw_socket);
		close(context.upper_listening_file_descriptor);
		unlink(context.socket_upper_path);
		return 1;
	}


	if (add_to_epoll(epoll_file_descriptor, context.raw_socket) == -1 ||
		add_to_epoll(epoll_file_descriptor, context.upper_listening_file_descriptor) == -1) {
		
		close(epoll_file_descriptor);
		close(context.raw_socket);
		close(context.upper_listening_file_descriptor);
		unlink(context.socket_upper_path);
		return 1;
	}

	printf("mipd running with MIP address %u\n", context.mip_address);

	while(1) {
		ready_count = epoll_wait(epoll_file_descriptor, events, MAX_EVENTS, -1);

		if (ready_count == -1) {
			perror("epoll_wait fail");
			break;
		}

		for (i = 0; i < ready_count; i++) {
			int ready_file_descriptor = events[i].data.fd;

			if(ready_file_descriptor == context.upper_listening_file_descriptor) {
				//new application is conncting
				int new_client = accept_upper_client(context.upper_listening_file_descriptor);

				if (new_client == -1) {
					continue;
				}

				//spec says only one upper layer process at a time
				if (context.upper_client_file_descriptor != -1) {
					printf("already have a upper layer client, rejecting new one\n");
					close(new_client);
					continue;
				}

				if (add_to_epoll(epoll_file_descriptor, new_client) == -1) {
					close(new_client);
					continue;
				}

				context.upper_client_file_descriptor = new_client;
			}
			else if (ready_file_descriptor == context.upper_client_file_descriptor) {
				//app sent something or disconnected
				int result = receive_upper_layer_message(&context);

				if (result == 0) {
					printf("local application disconnect\n");
					epoll_ctl(epoll_file_descriptor, EPOLL_CTL_DEL, ready_file_descriptor, NULL);
					close(ready_file_descriptor);
					context.upper_client_file_descriptor = -1;
				}
			}



			else if (ready_file_descriptor == context.raw_socket) {
				//frame from neighbor, arp or ping
				handle_raw_frame(&context);
			}
		}
	}
	
	/**
	 *Cleanup of sockets before exiting program.
	 */
	if (context.upper_client_file_descriptor != -1) {
		close(context.upper_client_file_descriptor);
		context.pending.active = false;
	}
	close(epoll_file_descriptor);
	close(context.upper_listening_file_descriptor);
	close(context.raw_socket);
	unlink(context.socket_upper_path);

	return 0;
}

