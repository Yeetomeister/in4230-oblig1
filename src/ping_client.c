#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>

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

	char expected_reply[1024];		//pong that we wait for
	uint8_t reply[1024];			//[source mip][text] received from daemon
	ssize_t received_bytes;
	struct timeval timeout;			//receive timeout for socket
	struct timespec send_time;		//when ping was sent
	struct timespec receive_time;		//when pong arrived
	double elapsed_ms;			//round trip time

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
		return 1;
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
	

	//snprintf is used to put string into buffer
	//+1 because mip address i a [0].
	payload_length = snprintf((char *)(buffer + 1), sizeof(buffer) - 1, "PING:%s", message);

	if(payload_length < 0 || payload_length >= (int)(sizeof(buffer) - 1)) {
		fprintf(stderr, "ping message too long\n");
		close(file_descriptor);
		return 1;
	}

	//reply must be PONG:message, so we build now and compare it later
	snprintf(expected_reply, sizeof(expected_reply), "PONG:%s", message);

	//receive timeout of 1 sec.
	timeout.tv_sec = 1;
	timeout.tv_usec = 0;

	if (setsockopt(file_descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1) {
		perror("setsockopt failed");
		close(file_descriptor);
		return 1;
	}

	clock_gettime(CLOCK_MONOTONIC, &send_time);

	sent_bytes = send(file_descriptor, buffer, (size_t)payload_length + 2, 0);

	if (sent_bytes == -1) {
		perror("failed to send bytes");
		close(file_descriptor);
		return 1;
	}

	while(1) {
		received_bytes = recv(file_descriptor, reply, sizeof(reply) -1, 0);

		if (received_bytes == -1) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				printf("timeout\n");
			}
			else {
				perror("recv failed");
			}
			close(file_descriptor);
			return 1;
		}

		reply[received_bytes] = '\0';

		if (strcmp((char *)(reply + 1), expected_reply) == 0) {
			break;
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &receive_time);

	elapsed_ms = (double)(receive_time.tv_sec - send_time.tv_sec) * 1000 + (double)(receive_time.tv_nsec - send_time.tv_nsec) / 1000000.0;

	printf("%s from MIP %u, time %.2f ms\n", (char *)(reply + 1), (unsigned int)reply[0], elapsed_ms);

	close(file_descriptor);
	return 0;

}
