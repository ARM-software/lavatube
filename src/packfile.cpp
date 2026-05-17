#include "jsoncpp/json/reader.h"
#include "packfile.h"
#include "zipc_utility.h"
#include "util.h"

#include <sys/sendfile.h>
#include <sys/mman.h>
#include <string>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <string.h>

static const uint16_t filename_length = 40;

static bool is_zip_pack(const std::string& packfile)
{
	return packfile.size() >= 4 && packfile.compare(packfile.size() - 4, 4, ".api") == 0;
}

struct fileentry {
	uint64_t pos;
	uint64_t len;
	char filename[filename_length];
};

static void packwrite(int fd, const void* ptr, uint_fast32_t size)
{
	size_t actually_written = write(fd, ptr, size);
	(void)actually_written;
	assert(actually_written == size);
}

static packed internal_packed_open(const std::string& packfile)
{
	struct stat st;
	packed pf;
	int flags = O_RDONLY;
#ifndef __ANDROID__
	flags |= O_NOATIME;
#endif
	pf.fd = openat(AT_FDCWD, packfile.c_str(), flags);
	if (pf.fd == -1)
	{
		FAIL("Cannot open pack file \"%s\": %s", packfile.c_str(), strerror(errno));
	}
	fstat(pf.fd, &st);
	if (!S_ISREG(st.st_mode)) FAIL("%s is not a regular file!", packfile.c_str());
	pf.filesize = st.st_size;
	// Parse header
	char signature[9]; // must be 'LAVATUBE\0' or 'LAVA0001\0'
	pf.read(signature, sizeof(signature));
	if (strcmp(signature, "LAVA0001") == 0)
	{
		pf.version = 1; // version 1 may have a sparse index and append-to-existing-packfile feature
	}
	else if (strcmp(signature, "LAVATUBE") == 0)
	{
		pf.version = 0;
	}
	else
	{
		FAIL("\"%s\" is not recognized as a lavatube trace file!", packfile.c_str());
	}
	pf.pack = packfile;
	return pf;
}

static std::vector<fileentry> get_packed_files(packed& pf)
{
	uint64_t idx_ptr_pos = 0;
	std::vector<fileentry> list;
	do
	{
		int16_t files;
		pf.read(&files, sizeof(files));
		for (int i = 0; i < files; i++)
		{
			fileentry f = {};
			pf.read(&f.pos, sizeof(f.pos));
			pf.read(&f.len, sizeof(f.len));
			pf.read(f.filename, filename_length);
			list.push_back(f);
		}
		if (pf.version >= 1)
		{
			pf.last_idx_ptr_pos = lseek(pf.fd, 0, SEEK_CUR);
			pf.read(&idx_ptr_pos, sizeof(idx_ptr_pos));
			if (idx_ptr_pos > 0)
			{
				unsigned long r = lseek(pf.fd, idx_ptr_pos, SEEK_SET);
				assert(r < pf.filesize);
				(void)r;
			}
		}
	} while (idx_ptr_pos != 0);
	return list;
}

bool pack_add(const std::string& newfile, const std::string& pack)
{
	if (is_zip_pack(pack))
	{
		const enum zipc_status status = zipc_add_file(pack, newfile);
		if (status != ZIPC_SUCCESS)
		{
			FAIL("Failed to add \"%s\" to \"%s\": %s", newfile.c_str(), pack.c_str(), zipc_strerror(status));
		}
		return true;
	}

	packed pfread = internal_packed_open(pack);
	if (pfread.version == 0) FAIL("%s is version 0 and cannot be appended!", pack.c_str());
	(void)get_packed_files(pfread); // just want pf.last_idx_ptr_pos to be set
	const uint64_t last_idx_ptr_pos = pfread.last_idx_ptr_pos;
	assert(last_idx_ptr_pos > 0);
	const uint64_t last_byte = pfread.size();
	pfread.close();
	int flags = O_WRONLY;
#ifndef __ANDROID__
	flags |= O_NOATIME;
#endif
	int filedesc = openat(AT_FDCWD, pack.c_str(), flags);
	if (filedesc == -1) FAIL("Failed to open for editing \"%s\": %s", pack.c_str(), strerror(errno));
	unsigned long r2 = lseek(filedesc, last_idx_ptr_pos, SEEK_SET);
	assert(r2 > 0);
	(void)r2;
	packwrite(filedesc, &last_byte, sizeof(last_byte));
	flags = O_RDONLY;
#ifndef __ANDROID__
	flags |= O_NOATIME;
#endif
	int readdesc = openat(AT_FDCWD, newfile.c_str(), flags);
	if (readdesc == -1) FAIL("Failed to open for reading \"%s\": %s", pack.c_str(), strerror(errno));
	struct stat st;
	fstat(readdesc, &st);
	if (!S_ISREG(st.st_mode)) FAIL("%s is not a regular file!", newfile.c_str());
	const uint64_t len = st.st_size;
	const uint16_t num = 1;
	const uint64_t next = 0;
	char name[filename_length];
	memset(name, 0, sizeof(name));
	strncpy(name, newfile.c_str(), filename_length - 1);
	const uint64_t pos = last_byte + sizeof(num) + 2 * sizeof(pos) + filename_length + sizeof(next) + 1;
	unsigned long rr = lseek(filedesc, last_byte, SEEK_SET);
	if (rr != last_byte) FAIL("Failed to seek to end of file");
	packwrite(filedesc, &num, sizeof(num));
	packwrite(filedesc, &pos, sizeof(pos));
	packwrite(filedesc, &len, sizeof(len));
	packwrite(filedesc, &name, sizeof(name));
	packwrite(filedesc, &next, sizeof(next));
	int r = sendfile(filedesc, readdesc, nullptr, len);
	if (r == -1) FAIL("Failed to add \"%s\" to \"%s\": %s", newfile.c_str(), pack.c_str(), strerror(errno));
	r = close(readdesc);
	assert(r != -1);
	r = close(filedesc);
	assert(r != -1);
	return true;
}

static bool pack_directory_zip(const std::string& pack, const std::string& directory, bool erase)
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
		int flags = O_RDONLY;
#ifndef __ANDROID__
		flags |= O_NOATIME;
#endif
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

packed packed_open(const std::string& inside, const std::string& pack)
{
	if (is_zip_pack(pack))
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

	packed pf = internal_packed_open(pack);
	pf.inside = inside;
	std::vector<fileentry> list = get_packed_files(pf);
	for (auto& v : list)
	{
		if (inside == v.filename)
		{
			pf.filesize = v.len;
			pf.consumed = 0;
			lseek(pf.fd, v.pos - 1, SEEK_SET);
			return pf;
		}
	}
	FAIL("Failed to find \"%s\" inside \"%s\"", inside.c_str(), pack.c_str());
	pf.close();
	return pf;
}

void packed_list(const std::string& pack)
{
	if (is_zip_pack(pack))
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
		for (const auto& name : list)
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
		return;
	}

	packed pf = internal_packed_open(pack);
	if (pf.size() == 0) return;
	std::vector<fileentry> list = get_packed_files(pf);
	if (list.empty()) return;
	printf("%-40s : %-20s : %-20s\n", "Filename", "Position", "Size");
	for (auto& v : list)
	{
		printf("%-40s : %20lu : %20lu\n", v.filename, (unsigned long)v.pos, (unsigned long)v.len);
	}
	pf.close();
}

std::vector<std::string> packed_files(const std::string& pack, const std::string& startsWith)
{
	if (is_zip_pack(pack))
	{
		return zipc_files(pack, startsWith);
	}

	std::vector<std::string> retval;
	packed pf = internal_packed_open(pack);
	std::vector<fileentry> list = get_packed_files(pf);
	for (auto& v : list)
	{
		if (startsWith.empty() || strncmp(v.filename, startsWith.c_str(), startsWith.size()) == 0)
		{
			retval.push_back(v.filename);
		}
	}
	pf.close();
	return retval;
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
	if (is_zip_pack(pack))
	{
		return pack_directory_zip(pack, directory, erase);
	}

	struct dirent **namelist;
	int n; // number of found entries in directory
	const char* signature = "LAVA0001";
	int flags = O_CREAT | O_TRUNC | O_WRONLY;
#ifndef __ANDROID__
	flags |= O_NOATIME;
#endif
	int filedesc = openat(AT_FDCWD, pack.c_str(), flags, S_IRWXU | S_IRGRP | S_IROTH);
	if (filedesc == -1) FAIL("Failed to create \"%s\": %s", pack.c_str(), strerror(errno));
	packwrite(filedesc, signature, 9);
	n = scandir(directory.c_str(), &namelist, NULL, alphasort);
	if (n < 0) FAIL("Failed to scan \"%s\": %s", directory.c_str(), strerror(errno));
	int16_t j = 0; // real count without the .dotfiles
	for (int i = 0; i < n; i++)
	{
		if (namelist[i]->d_name[0] == '.') continue;
		j++;
	}
	packwrite(filedesc, &j, sizeof(j));
	for (int i = 0; i < n; i++) // fill out index with dummy values for now, we'll fix it below
	{
		if (namelist[i]->d_name[0] == '.') continue;
		const uint64_t pos = 0;
		const uint64_t len = 0;
		char filename[filename_length];
		memset(filename, 0, sizeof(filename));
		packwrite(filedesc, &pos, sizeof(pos));
		packwrite(filedesc, &len, sizeof(len));
		packwrite(filedesc, filename, sizeof(filename));
	}
	uint64_t zero = 0;
	packwrite(filedesc, &zero, sizeof(zero)); // pointer to next index
	j = 0; // real count without the . and ..
	for (int i = 0; i < n; i++) // fill out index with dummy values for now, we'll fix it below
	{
		if (namelist[i]->d_name[0] == '.') continue;
		std::string name = directory + "/" + std::string(namelist[i]->d_name);
		const uint64_t pos = lseek(filedesc, 0, SEEK_CUR) + 1;
		flags = O_RDONLY;
#ifndef __ANDROID__
		flags |= O_NOATIME;
#endif
		int fd = openat(AT_FDCWD, name.c_str(), flags);
		if (fd == -1) FAIL("Failed to open \"%s\": %s", name.c_str(), strerror(errno));
		struct stat st;
		int r = fstat(fd, &st);
		if (r == -1) FAIL("Failed to stat \"%s\": %s", name.c_str(), strerror(errno));
		r = sendfile(filedesc, fd, nullptr, st.st_size);
		if (r == -1) FAIL("Failed to add \"%s\" to \"%s\": %s", name.c_str(), pack.c_str(), strerror(errno));
		const uint64_t len = st.st_size;
		const unsigned wpos = 9 + 2 + j * (sizeof(uint64_t) * 2 + filename_length);
		lseek(filedesc, wpos, SEEK_SET);
		packwrite(filedesc, &pos, sizeof(pos));
		packwrite(filedesc, &len, sizeof(len));
		assert(strlen(namelist[i]->d_name) < filename_length);
		packwrite(filedesc, namelist[i]->d_name, strlen(namelist[i]->d_name));
		lseek(filedesc, 0, SEEK_END);
		r = close(fd);
		assert(r != -1);
		j++;
	}
	for (int i = 0; i < n; i++) // clean up
	{
		if (erase && namelist[i]->d_name[0] != '.')
		{
			std::string name = directory + "/" + std::string(namelist[i]->d_name);
			int res = remove(name.c_str());
			if (res == -1) ELOG("Could not remove %s: %s", name.c_str(), strerror(errno));
		}
		free(namelist[i]);
	}
	if (erase)
	{
		int res = rmdir(directory.c_str());
		if (res == -1) ELOG("Could not remove %s: %s", directory.c_str(), strerror(errno));
	}
	free(namelist);
	fsync(filedesc);
	int r = close(filedesc);
	assert(r != -1);
	return (r != -1);
}

bool unpack_directory(const std::string& pack, const std::string& directory)
{
	int r = mkdir(directory.c_str(), 0777);
	if (r == -1 && errno != EEXIST) ELOG("Could not create \"%s\": %s", directory.c_str(), strerror(errno));
	if (is_zip_pack(pack))
	{
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
			int flags = O_CREAT | O_TRUNC | O_WRONLY;
#ifndef __ANDROID__
			flags |= O_NOATIME;
#endif
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

	packed pf = internal_packed_open(pack);
	std::vector<fileentry> list = get_packed_files(pf);
	for (auto& v : list)
	{
		std::string name = directory + "/" + std::string(v.filename);
		int flags = O_CREAT | O_TRUNC | O_WRONLY;
#ifndef __ANDROID__
		flags |= O_NOATIME;
#endif
		int fd = openat(AT_FDCWD, name.c_str(), flags, 0664);
		if (fd == -1) FAIL("Failed to open \"%s\": %s", name.c_str(), strerror(errno));
		off_t off = v.pos - 1;
		r = sendfile(fd, pf.fd, &off, v.len);
		if (r == -1) FAIL("Failed to add \"%s\" to \"%s\": %s", name.c_str(), pack.c_str(), strerror(errno));
		r = close(fd);
		assert(r != -1);
	}
	pf.close();
	return true;
}
