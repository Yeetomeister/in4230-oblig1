#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

int main(int argc, char *argv[]){
	struct daemon_context context = {
		.debug = false,
		.mip_address = 0,
		.raw_file_descriptor = -1,
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

	printf("MIPD argumnt parsing success\n");
	printf("UNIX socket path: %s\n", context.socket_upper_path);
	printf("MIP address: %d\n", context.mip_address);

	if (context.debug){
		printf("Debug mode active\n");
	}

	return 0;
}

