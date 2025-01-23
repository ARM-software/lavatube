#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/sendfile.h>
#include <vector>
#include <map>
#include "packfile.h"

int main(int argc, char* argv[])
{
	if (argc < 3 || (argc < 4 && strcmp(argv[1], "list") != 0 && strcmp(argv[1], "check") != 0)
	    || (argc > 3 && (strcmp(argv[1], "list") == 0 || strcmp(argv[1], "check") == 0)))
	{
		fprintf(stdout, "Usage:\n");
		fprintf(stdout, "\t%s pack <output file> <input directory>\n", argv[0]);
		fprintf(stdout, "\t%s unpack <output directory> <input packaged file>\n", argv[0]);
		fprintf(stdout, "\t%s extract <output file> <input packaged file>\n", argv[0]);
		fprintf(stdout, "\t%s add <input file> <input packaged file>\n", argv[0]);
		fprintf(stdout, "\t%s list <input packaged file>\n", argv[0]);
		fprintf(stdout, "\t%s print <target file> <input packaged file>\n", argv[0]); // pretty print a JSON
		fprintf(stdout, "\t%s check <input packaged file>\n", argv[0]); // sanitycheck contents
		return 0;
	}
	else if (strcmp(argv[1], "check") == 0)
	{
		std::map<std::string, bool> map;
		std::vector<std::string> files = packed_files(argv[2]);
		int threads_bin = 0;
		int threads_json = 0;
		for (const std::string& s : files)
		{
			map[s] = true;
			if (strncmp(s.c_str(), "thread_", 6) == 0) threads_bin++;
			if (strncmp(s.c_str(), "frames_", 6) == 0) threads_json++;
		}
		assert(threads_bin > 0);
		assert(threads_bin == threads_json);
		assert(map.at("limits.json") == true);
		assert(map.at("dictionary.json") == true);
		assert(map.at("metadata.json") == true);
		assert(map.at("tracking.json") == true);
		assert(map.at("frames_0.json") == true);
		(void)threads_bin;
		(void)threads_json;
		printf("Success\n");
		return 0;
	}
	else if (strcmp(argv[1], "pack") == 0)
	{
		if (!pack_directory(argv[2], argv[3], false))
		{
			FAIL("Failed to pack directory \"%s\" into \"%s\"", argv[3], argv[2]);
		}
		return 0;
	}
	else if (strcmp(argv[1], "add") == 0)
	{
		if (!pack_add(argv[2], argv[3]))
		{
			FAIL("Failed to add \"%s\" into \"%s\"", argv[3], argv[2]);
		}
		return 0;
	}
	else if (strcmp(argv[1], "unpack") == 0)
	{
		if (!unpack_directory(argv[3], argv[2]))
		{
			FAIL("Failed to unpack file \"%s\" into \"%s\"", argv[3], argv[2]);
		}
		return 0;
	}
	else if (strcmp(argv[1], "print") == 0)
	{
		Json::Value js = packed_json(argv[2], argv[3]);
		printf("%s", js.toStyledString().c_str());
		return 0;
	}
	else if (strcmp(argv[1], "extract") == 0)
	{
		packed pf = packed_open(argv[2], argv[3]);
		int fd = open(argv[2], O_CREAT | O_TRUNC | O_WRONLY | O_NOATIME, 0664);
		if (fd == -1) FAIL("Failed to create target %s: %s", argv[2], strerror(errno));
		int res = sendfile(fd, pf.fd, nullptr, pf.filesize);
		if (res == -1) FAIL("Failed to write out the file: %s\n", strerror(errno));
		pf.close();
		close(fd);
		return 0;
	}
	else if (strcmp(argv[1], "list") == 0)
	{
		packed_list(argv[2]);
		return 0;
	}
	fprintf(stderr, "Unrecognized command: %s\n", argv[1]);
	return -1;
}
