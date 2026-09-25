#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>

static void print_usage(const char *program_name) {
	printf("usage: %s [-h] <socket_lower> <message> <destination_host>\n", program_name);
}

//Reuse of mipd, still havent figured out copy paste or "yanking" in VIM so this is nice
static int parse_mip_address(const char *text, uint8_t *result) {
	char *end;
	unsigned long value;

	value = strtoul(text, &end, 10);

	if (text[0] == '\0' || *end != '\0' || value > 254) {
		return -1;
	}

	*result = (uint8_t)value;
	return 0;
}

int main(int argc, char *argv[]){
	struct sockaddr_un address;
	uint8_t buffer[1024];
	uint8_t destination_address;
	const char *socket_path;
	const char *message;
	int file_descriptor;
	int payload_length;
	ssize_t sent_bytes;

	if (argc == 2 && strcmp(argv[1], "-h") == 0) {
		print_usage(argv[0]);
		return 0;
	}
	if (argc != 4) {
		print_usage(argv[0]);
		return 1;
	}

	socket_path = argv[1];
	message = argv[2];

	if (parse_mip_address(argv[3], &destination_address) == -1) {
		fprintf(stderr, "Destination not valid MIP address\n");
		return -1;
	}

	file_descriptor = socket(AF_UNIX, SOCK_SEQPACKET, 0);

	if (file_descriptor == -1) {
		perror("failed to get socket from file dscrptr");
		return 1;
	}

	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;

	if (strlen(socket_path) >= sizeof(address.sun_path)) {
		fprintf(stderr, "Unix socket path too long\n");
		close(file_descriptor);
		return 1;
	}

	strncpy(address.sun_path, socket_path, sizeof(address.sun_path) - 1);

	if(connect(file_descriptor, (struct sockaddr *)&address, sizeof(address)) == -1) {
		perror("failed connection");
		close(file_descriptor);
		return 1;
	}

	//TODO use established socket to send data to daempn
	
	buffer[0] = destination_address;
	
	/**
	 *snprintf used to put string into buffer, requires char *
	 * + 1 as mip address is at index 0 and subsequently we reduce the allowed size put into buffer by 1.
	 */
	payload_length = snprintf((char *)(buffer + 1), sizeof(buffer) - 1, "PING:%s", message);

	if(message_length < 0 || payload_length > (int)(sizeof(buffer) - 1)) {
		fprintf(stderr, "ping message is too long\n");
		close(file_descriptor);
		return 1;
	}

	//Track what we send
	sent_bytes = send(file_descriptor, buffer, (size_t)payload_length + 1, 0);

	if (sent_bytes == -1) {
		perror("failed to send bytes");
		close(file_descriptor);
		return 1;
	}

	printf("sent ping message:%s to mip address %u.\n", message, (unsigned int)destination_address);

	return 0;

}
