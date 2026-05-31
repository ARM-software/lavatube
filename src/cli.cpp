#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include <string>

#include "lavatube.h"

static bool verbose = false;

void usage()
{
	printf("lava-cli %d.%d.%d-" RELTYPE " command line options\n", LAVATUBE_VERSION_MAJOR, LAVATUBE_VERSION_MINOR, LAVATUBE_VERSION_PATCH);
	printf("lava-cli [options] <status|continue|stop|step [packets N|calls N]|goto N|params>\n");
	printf("-h/--help              This help\n");
	printf("-v/--verbose           Verbose output\n");
	printf("-P/--port PORT         Port number (default %d)\n", (int)p__port);
	printf("-H/--host HOST         Host name\n");
#ifndef NDEBUG
	printf("-d/--debug level       Set debug level [0,1,2,3]\n");
	printf("-df/--debugfile FILE   Output debug output to the given file\n");
#endif
	exit(-1);
}

int main(int argc, char **argv)
{
	std::string hostname = "localhost";
	std::string keyword;
	int port = p__port;
	int remaining = argc - 1; // zeroth is name of program

	for (int i = 1; i < argc; i++)
	{
		if (match(argv[i], "-h", "--help", remaining))
		{
			usage();
		}
		else if (match(argv[i], "-d", "--debug", remaining))
		{
			p__debug_level = get_int(argv[++i], remaining);
		}
		else if (match(argv[i], "-P", "--port", remaining))
		{
			port = get_int(argv[++i], remaining);
		}
		else if (match(argv[i], "-H", "--host", remaining))
		{
			hostname = get_str(argv[++i], remaining);
		}
		else if (match(argv[i], "-v", "--verbose", remaining))
		{
			verbose = true;
		}
		else if (match(argv[i], "-df", "--debugfile", remaining))
		{
			std::string val = get_str(argv[++i], remaining);
			if (p__debug_destination != stdout) ABORT("We already have a different debug file destination!");
			p__debug_destination = fopen(val.c_str(), "w");
		}
		else
		{
			keyword = get_str(argv[i], remaining);
			while (remaining > 0)
			{
				keyword += " " + get_str(argv[++i], remaining);
			}
		}
	}

	if (keyword.empty()) usage();

	if (verbose)
	{
		printf("Connecting to %s:%d\n", hostname.c_str(), port);
	}

	const int fd = lava_tcp_connect(hostname, port);
	if (!lava_tcp_send_all(fd, keyword + "\n"))
	{
		DIE("Failed to send %s command to %s:%d: %s", keyword.c_str(), hostname.c_str(), port, strerror(errno));
	}
	const std::string response = lava_tcp_receive_all(fd);
	close(fd);

	printf("%s", response.c_str());
	if (response.empty() || response == "ERROR\n" || response == "ERROR") return 1;

	return 0;
}
