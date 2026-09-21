#include <stdio.h>
#include <string.h>

#include "../include/mipd.h"

void print_usage(const char *program_name){
	printf("usage: %s [-h] [-d] <socket_upper> <MIP address>\n", program_name);
}

int main(int argc, char *argv[]){
	struct daemon_context context = {
		.debug = false,
		.mip_address = 0,
		.raw_file_descriptor = -1,
		.upper_listen_file_descriptor = -1,
		.upper_client_file_descriptor = -1
	};
	
	(void)context;

	if (argc == 2 && strcmp(argv[1], "-h") == 0) {
		print_usage(argv[0]);
		return 0;
	}
	else if (argc == 4 && strcmp(argv[1], "-d") == 0) {
		context.debug = true;
		context.upper.listen_file_descriptor = argc[2];
		context.mip_address = argc[3];

		//start loop

		return 0;
	}

	else if (argc == 3){
		context.upper_listen_file_descriptor = argc[2];
		context.mip_address = argc[3];

		//start loop

		return 0;
	}
	else{
		return 1;
	}

	return 0;
}

