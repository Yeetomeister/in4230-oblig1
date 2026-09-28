#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

//Includes for initial unix socket attempt.
#include <sys/epoll.h> //use epoll as we want a fifo queue of incoming requests for the server aka node b
#include <sys/socket.h>
#include <sys/un.h>

//raw sockets
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <sys/socket.h>

//eth
#include <ifaddrs.h>

#include "../include/mipd.h"

void print_usage(const char *program_name){
	printf("usage: %s [-h] [-d] <socket_upper> <MIP address>\n", program_name);
}

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

	printf("waiting for message\n");//accept will hang, so nice to know we reached the correct point

	client_file_descriptor = accept(listening_file_descriptor, NULL, NULL);

	if (client_file_descriptor == -1) {
		perror("failed to accept listening_file_descriptor\n");
		return -1;
	}

	printf("local client connected\n");

	return client_file_descriptor;

}

/**
 *Receive message and dst host from upper ping client.
 *The docuemntation says that the command line arguments should be message then mip address. But switching this in the internal logic makes it easier to separate the address from message as the addres has constant size. This will surely make problems later, but seems like the best solution
 *
 */
static int receive_upper_layer_message(int client_file_descriptor) {
	uint8_t buffer[1024];
	ssize_t received_bytes;

	received_bytes = recv(client_file_descriptor, buffer, sizeof(buffer), 0);

	if (received_bytes == 0) {
		printf("Client closed connection without sending data\n");
		return -1;
	}

	//Assume specified command line arguments are enforced elsewhere, bad practice I know
	if (received_bytes == 1) {
		printf("Received address but no message.\n");
		return -1;
	}

	printf("received message destined for address: %u\n", (unsigned int)buffer[0]);
	printf("Payload: ");
	//write buffer out as raw data
	fwrite(buffer + 1, 1, (size_t)received_bytes -1, stdout);
	printf("\n");
	
	return 0;
}

/**
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
 *the context supplied will receive the discovered interfaces to their sockaddr_ll interface list.
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
		printf("MIP address has to be number between 0 and 255\n");
		return 1;
	}

	//test implemented header encoding and decoding
	struct mip_header example = {
		.destination = 2,
		.source = 1,
		.ttl = 1,
		.sdu_length_words = 2,
		.sdu_type = MIP_SDU_TYPE_PING
	};

	struct mip_header decoded;
	uint8_t bytes[MIP_HEADER_SIZE];
	unsigned int i;

	if (encode_mip_header(&example, bytes) == -1 || decode_mip_header(bytes, &decoded) == -1) {
		fprintf(stderr, "encode or decode failed");
		return 1;
	}

	printf("encoded header:");
	for (i = 0; i < MIP_HEADER_SIZE; i++) {
		printf(" %02x", (unsigned int)(bytes[i]));
	};
	printf("\nDecoded destination=%u source=%u TTL=%u length=%u words type=%u \n",
		(unsigned int)decoded.destination,
		(unsigned int)decoded.source,
		(unsigned int)decoded.ttl,
		(unsigned int)decoded.sdu_length_words,
		(unsigned int)decoded.sdu_type);

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

	context.upper_client_file_descriptor = accept_upper_client(context.upper_listening_file_descriptor);



	if (context.upper_listening_file_descriptor == -1) {
		close(context.upper_listening_file_descriptor);
		unlink(context.socket_upper_path);
		return 1;
	}


	if (receive_upper_layer_message(context.upper_client_file_descriptor) == -1) {
		close(context.upper_client_file_descriptor);
		close(context.upper_listening_file_descriptor);
		unlink(context.socket_upper_path);
		return -1;
	}

	printf("Unix listening socket was created\n");

	printf("MIPD argumnt parsing success\n");
	printf("UNIX socket path: %s\n", context.socket_upper_path);
	printf("MIP address: %d\n", context.mip_address);

	if (context.debug){
		printf("Debug mode active\n");
	}

	/**
	 *Cleanup of sockets before exiting program.
	 */
	close(context.upper_client_file_descriptor);
	close(context.upper_listening_file_descriptor);
	close(context.raw_socket);
	unlink(context.socket_upper_path);

	return 0;
}

