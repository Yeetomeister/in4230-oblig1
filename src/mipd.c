#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

//Includes for initial unix socket attempt.
#include <sys/epoll.h> //use epoll as we want a fifo queue of incoming requests for the server aka node b
#include <sys/socket.h>
#include <sys/un.h>

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
		return 1;
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

	printf("waiting for message");//accept will hang, so nice to know we reached the correct point

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
	fwrite(buffer +1, 1, (size_t)received_bytes -1, stdout);
	printf("\n");
	
	return 0;
}



int main(int argc, char *argv[]){
	struct daemon_context context = {
		.debug = false,
		.mip_address = 0,
		.raw_file_descriptor = -1,
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

	context.socket_upper_path = socket_path;

	context.upper_listening_file_descriptor = create_upper_listening_socket(context.socket_upper_path);

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
	unlink(context.socket_upper_path);

	return 0;
}

