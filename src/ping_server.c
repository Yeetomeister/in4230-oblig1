#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>

#include <sys/socket.h>
#include <sys/un.h>


/*
 *print how to use the program
 *
 *program_name: argv[0]
 *
 *no return, only print
 */
static void print_usage(const char *program_name) {
	printf("usage: %s [-h] <socket_lower>\n", program_name);
}

/*
 *pretty much same code as in client, but moved out to it's own functoin
 *connect to local mip daemon over unix socket.
 *
 *socket_path: path that the daemon bound its unix socket to
 *
 * returns the connected file descriptor or -1 on error
 */
static int connect_to_daemon(const char *socket_path) {
	struct sockaddr_un address;
	int file_descriptor;

	file_descriptor = socket(AF_UNIX, SOCK_SEQPACKET, 0);

	if (file_descriptor == -1) {
		perror("failed at establishing socket");
		return -1;
	}

	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;

	if (strlen(socket_path) >= sizeof(address.sun_path)) {
		fprintf(stderr, "unix socket path is too long.\n");
		close(file_descriptor);
		return -1;
	}

	strncpy(address.sun_path, socket_path, sizeof(address.sun_path) - 1);

	if (connect(file_descriptor, (struct sockaddr *)&address, sizeof(address)) == -1) {
		perror("failed to connect");
		close(file_descriptor);
		return -1;
	}

	return file_descriptor;
}

int main(int argc, char *argv[]) {
	int file_descriptor;
	uint8_t buffer[1024];		//[source mip][text] from daemon
	uint8_t reply [1024];		//[destinaion][pong:text] back to daemon
	ssize_t received_bytes;
	int reply_length;
	const char *text;

	if(argc == 2 && strcmp(argv[1], "-h") == 0){
		print_usage(argv[0]);
		return 0;
	}

	if (argc != 2) {
		print_usage(argv[0]);
		return 1;
	}

	file_descriptor = connect_to_daemon(argv[1]);

	if (file_descriptor == -1) {
		return 1;
	}

	printf("ping server connected, waiting for ping\n");

	while(1) {
		received_bytes = recv(file_descriptor, buffer, sizeof(buffer) -1, 0);

		if (received_bytes == -1) {
			perror("recv");
			break;
		}

		if (received_bytes == 0){
			printf("daemon closed connection");
			break;
		}

		//add null terminator in case sender has not included
		buffer[received_bytes] = '\0';
		text = (const char *)(buffer + 1);

		printf("received from MIP %u: %s\n", (unsigned int)buffer[0], text);

		//only answer ping
		if (strncmp(text, "PING:", 5) != 0) {
			continue;
		}

		//answer go back to sender, so source becomes dst
		reply[0] = buffer[0];
		reply_length = snprintf((char *)(reply + 1), sizeof(reply) - 1, "PONG:%s", text + 5);

		if (reply_length < 0 || reply_length >= (int)(sizeof(reply) - 1)) {
			fprintf(stderr, "reply too long\n");
			continue;
		}

		if (send(file_descriptor, reply, (size_t)reply_length + 2, 0) == -1) {
			perror("send failed");
			break;
		}
	}
	close(file_descriptor);
	return 0;
}
