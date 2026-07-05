#include "jsoncpp/json/reader.h"
#include "packfile.h"
#include "zipc_utility.h"
#include "util.h"

#include <algorithm>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>

bool pack_add(const std::string& newfile, const std::string& pack)
{
	const enum zipc_status status = zipc_add_file(pack, newfile);
	if (status != ZIPC_SUCCESS)
	{
		FAIL("Failed to add \"%s\" to \"%s\": %s", newfile.c_str(), pack.c_str(), zipc_strerror(status));
	}
	return true;
}

packed packed_open(const std::string& inside, const std::string& pack)
{
	enum zipc_status status = ZIPC_SUCCESS;
	packed pf;
	pf.pack = pack;
	pf.inside = inside;
	pf.zip_handle = zipc_open(pack.c_str(), "r", &status);
	if (!pf.zip_handle) FAIL("Cannot open zip file \"%s\": %s", pack.c_str(), zipc_strerror(status));
	pf.zip_mapping = zipc_map_read(pf.zip_handle, inside.c_str(), &status);
	if (!pf.zip_mapping.data)
	{
		zipc_close(pf.zip_handle);
		pf.zip_handle = nullptr;
		FAIL("Failed to open \"%s\" inside \"%s\": %s", inside.c_str(), pack.c_str(), zipc_strerror(status));
	}
	pf.filesize = pf.zip_mapping.size;
	return pf;
}

void packed_list(const std::string& pack)
{
	enum zipc_status status = ZIPC_SUCCESS;
	zipc* archive = zipc_open(pack.c_str(), "r", &status);
	if (!archive) FAIL("Cannot open zip file \"%s\": %s", pack.c_str(), zipc_strerror(status));
	std::vector<std::string> list = zipc_files(pack);
	if (list.empty())
	{
		zipc_close(archive);
		return;
	}
	printf("%-40s : %-20s : %-20s\n", "Filename", "Position", "Size");
	for (const std::string& name : list)
	{
		uint64_t size = 0;
		status = zipc_filesize(archive, name.c_str(), &size);
		if (status != ZIPC_SUCCESS)
		{
			zipc_close(archive);
			FAIL("Failed to stat \"%s\" inside \"%s\": %s", name.c_str(), pack.c_str(), zipc_strerror(status));
		}
		printf("%-40s : %20d : %20lu\n", name.c_str(), 0, (unsigned long)size);
	}
	zipc_close(archive);
}

std::vector<std::string> packed_files(const std::string& pack, const std::string& startsWith)
{
	return zipc_files(pack, startsWith);
}

Json::Value packed_json(const std::string& inside, const std::string& pack)
{
	packed pf = packed_open(inside, pack);
	std::string data;
	data.resize(pf.filesize);
	pf.read(&data[0], data.size());
	Json::Value value;
	Json::Reader reader;
	bool success = reader.parse(data, value, false);
	if (!success) FAIL("Failed to parse JSON data");
	pf.close();
	return value;
}

void erase_directory(const std::string& directory)
{
	struct dirent **namelist;
	int n = scandir(directory.c_str(), &namelist, NULL, alphasort);
	if (n < 0) { ELOG("Failed to scan \"%s\": %s", directory.c_str(), strerror(errno)); return; }
	for (int i = 0; i < n; i++)
	{
		if (namelist[i]->d_name[0] != '.')
		{
			std::string name = directory + "/" + std::string(namelist[i]->d_name);
			int res = remove(name.c_str());
			if (res == -1) ELOG("Could not remove %s: %s", name.c_str(), strerror(errno));
		}
		free(namelist[i]);
	}
	free(namelist);
	int res = rmdir(directory.c_str());
	if (res == -1) ELOG("Could not remove %s: %s", directory.c_str(), strerror(errno));
}

bool pack_directory(const std::string& pack, const std::string& directory, bool erase)
{
	struct dirent **namelist;
	int n = scandir(directory.c_str(), &namelist, NULL, alphasort);
	if (n < 0) FAIL("Failed to scan \"%s\": %s", directory.c_str(), strerror(errno));

	enum zipc_status status = ZIPC_SUCCESS;
	zipc* archive = zipc_open(pack.c_str(), "w", &status);
	if (!archive) FAIL("Failed to create \"%s\": %s", pack.c_str(), zipc_strerror(status));

	for (int i = 0; i < n; i++)
	{
		if (namelist[i]->d_name[0] == '.') continue;

		const std::string name = directory + "/" + std::string(namelist[i]->d_name);
		int flags = O_RDONLY | default_file_flags;
		int fd = openat(AT_FDCWD, name.c_str(), flags);
		if (fd == -1) FAIL("Failed to open \"%s\": %s", name.c_str(), strerror(errno));

		struct stat st;
		int r = fstat(fd, &st);
		if (r == -1) FAIL("Failed to stat \"%s\": %s", name.c_str(), strerror(errno));
		if (!S_ISREG(st.st_mode)) FAIL("%s is not a regular file!", name.c_str());

		if (st.st_size == 0)
		{
			status = zipc_write(archive, namelist[i]->d_name, 0, nullptr);
		}
		else
		{
			void* mapped = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
			if (mapped == MAP_FAILED) FAIL("Failed to map \"%s\": %s", name.c_str(), strerror(errno));
			status = zipc_write(archive, namelist[i]->d_name, st.st_size, mapped);
			int unmap_result = munmap(mapped, st.st_size);
			if (unmap_result != 0) FAIL("Failed to unmap \"%s\": %s", name.c_str(), strerror(errno));
		}

		r = close(fd);
		assert(r != -1);
		if (status != ZIPC_SUCCESS) FAIL("Failed to add \"%s\" to \"%s\": %s", name.c_str(), pack.c_str(), zipc_strerror(status));
	}

	status = zipc_close(archive);
	if (status != ZIPC_SUCCESS) FAIL("Failed to close \"%s\": %s", pack.c_str(), zipc_strerror(status));

	for (int i = 0; i < n; i++)
	{
		if (erase && namelist[i]->d_name[0] != '.')
		{
			std::string name = directory + "/" + std::string(namelist[i]->d_name);
			int res = remove(name.c_str());
			if (res == -1) ELOG("Could not remove %s: %s", name.c_str(), strerror(errno));
		}
		free(namelist[i]);
	}
	free(namelist);
	if (erase)
	{
		int res = rmdir(directory.c_str());
		if (res == -1) ELOG("Could not remove %s: %s", directory.c_str(), strerror(errno));
	}
	return true;
}

bool unpack_directory(const std::string& pack, const std::string& directory)
{
	int r = mkdir(directory.c_str(), 0777);
	if (r == -1 && errno != EEXIST) ELOG("Could not create \"%s\": %s", directory.c_str(), strerror(errno));
	const std::vector<std::string> files = zipc_files(pack);
	enum zipc_status status = ZIPC_SUCCESS;
	zipc* archive = zipc_open(pack.c_str(), "r", &status);
	if (!archive) FAIL("Cannot open zip file \"%s\": %s", pack.c_str(), zipc_strerror(status));
	std::vector<char> buffer(1024 * 1024);
	for (const std::string& name : files)
	{
		uint64_t size = 0;
		status = zipc_filesize(archive, name.c_str(), &size);
		if (status != ZIPC_SUCCESS)
		{
			zipc_close(archive);
			FAIL("Failed to stat \"%s\" inside \"%s\": %s", name.c_str(), pack.c_str(), zipc_strerror(status));
		}
		std::string target = directory + "/" + name;
		int flags = O_CREAT | O_TRUNC | O_WRONLY | default_file_flags;
		int fd = openat(AT_FDCWD, target.c_str(), flags, 0664);
		if (fd == -1)
		{
			zipc_close(archive);
			FAIL("Failed to open \"%s\": %s", target.c_str(), strerror(errno));
		}
		zipc_mapping mapping = zipc_map_read(archive, name.c_str(), &status);
		if (!mapping.data)
		{
			close(fd);
			zipc_close(archive);
			FAIL("Failed to read \"%s\" inside \"%s\": %s", name.c_str(), pack.c_str(), zipc_strerror(status));
		}
		uint64_t remaining = size;
		const char* ptr = static_cast<const char*>(mapping.data);
		while (remaining > 0)
		{
			const size_t chunk = std::min<uint64_t>(buffer.size(), remaining);
			memcpy(buffer.data(), ptr, chunk);
			ptr += chunk;
			size_t written = 0;
			while (written < chunk)
			{
				const ssize_t res = write(fd, buffer.data() + written, chunk - written);
				if (res == -1 && errno == EINTR) continue;
				if (res <= 0)
				{
					zipc_unmap_read(archive, mapping);
					close(fd);
					zipc_close(archive);
					FAIL("Failed to write \"%s\": %s", target.c_str(), strerror(errno));
				}
				written += res;
			}
			remaining -= chunk;
		}
		zipc_unmap_read(archive, mapping);
		r = close(fd);
		assert(r != -1);
	}
	zipc_close(archive);
	return true;
}
