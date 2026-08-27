#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <algorithm>

struct usb_device_info
{
	std::string sysfs_path;
	std::string port_name;
	std::string id_vendor;
	std::string id_product;
	std::string manufacturer;
	std::string product;
	std::string serial;
	std::string version;
	std::string speed_str;
	double speed_mbps = 0.0;
	int busnum = 0;
	int devnum = 0;
	bool has_adb_interface = false;
	bool is_android = false;
	std::string identification_reason;
	std::string host_port_max_speed;
};

static std::string trim_string(const std::string& str)
{
	size_t first = str.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
	{
		return "";
	}
	size_t last = str.find_last_not_of(" \t\r\n");
	return str.substr(first, (last - first + 1));
}

static std::string to_lower_string(const std::string& str)
{
	std::string result = str;
	for (size_t i = 0; i < result.size(); i++)
	{
		result[i] = (char)tolower((unsigned char)result[i]);
	}
	return result;
}

static std::string read_sysfs_attr(const std::string& dev_path, const std::string& attr)
{
	std::string full_path = dev_path + "/" + attr;
	std::ifstream file(full_path.c_str());
	if (!file.is_open())
	{
		return "";
	}
	std::string value;
	std::getline(file, value);
	return trim_string(value);
}

static bool check_adb_interface(const std::string& dev_path)
{
	DIR* dir = opendir(dev_path.c_str());
	if (dir == nullptr)
	{
		return false;
	}

	bool found_adb = false;
	struct dirent* entry = nullptr;
	while ((entry = readdir(dir)) != nullptr)
	{
		if (strchr(entry->d_name, ':') != nullptr)
		{
			std::string iface_path = dev_path + "/" + entry->d_name;
			std::string iface_class = read_sysfs_attr(iface_path, "bInterfaceClass");
			std::string iface_subclass = read_sysfs_attr(iface_path, "bInterfaceSubClass");
			std::string iface_protocol = read_sysfs_attr(iface_path, "bInterfaceProtocol");

			if (to_lower_string(iface_class) == "ff" &&
			    to_lower_string(iface_subclass) == "42" &&
			    to_lower_string(iface_protocol) == "01")
			{
				found_adb = true;
				break;
			}
		}
	}
	closedir(dir);
	return found_adb;
}

static std::string detect_host_port_max_speed(const std::string& dev_path)
{
	std::string speed = read_sysfs_attr(dev_path + "/port/peer/../..", "speed");
	if (!speed.empty())
	{
		return speed;
	}

	char canon[PATH_MAX];
	std::string peer_link = dev_path + "/port/peer";
	if (realpath(peer_link.c_str(), canon) != nullptr)
	{
		std::string peer_path(canon);
		std::string hub_path = peer_path + "/../..";
		char canon_hub[PATH_MAX];
		if (realpath(hub_path.c_str(), canon_hub) != nullptr)
		{
			return read_sysfs_attr(std::string(canon_hub), "speed");
		}
	}
	return "";
}

static bool evaluate_android_device(usb_device_info* info)
{
	info->is_android = false;

	if (info->has_adb_interface)
	{
		info->is_android = true;
		info->identification_reason = "ADB debugging interface active (Class 0xFF, SubClass 0x42, Protocol 0x01)";
		return true;
	}

	std::string vid = to_lower_string(info->id_vendor);
	std::string pid = to_lower_string(info->id_product);

	if (vid == "18d1")
	{
		info->is_android = true;
		if (pid >= "2d00" && pid <= "2d05")
		{
			info->identification_reason = "Google Android Open Accessory (AOA) mode";
		}
		else
		{
			info->identification_reason = "Google Vendor ID (0x18D1)";
		}
		return true;
	}

	const char* android_vids[] = {
		"04e8", // Samsung
		"2717", // Xiaomi
		"12d1", // Huawei
		"1004", // LG
		"22d9", // Oppo / OnePlus
		"22b8", // Motorola
		"0fce", // Sony
		"0bb4", // HTC
		"0b05", // Asus
		nullptr
	};

	for (int i = 0; android_vids[i] != nullptr; i++)
	{
		if (vid == android_vids[i])
		{
			info->is_android = true;
			info->identification_reason = "Known Android OEM Vendor ID (0x" + info->id_vendor + ")";
			return true;
		}
	}

	std::string mfg_lower = to_lower_string(info->manufacturer);
	std::string prod_lower = to_lower_string(info->product);
	if (mfg_lower.find("android") != std::string::npos ||
	    prod_lower.find("android") != std::string::npos ||
	    prod_lower.find("pixel") != std::string::npos ||
	    prod_lower.find("galaxy") != std::string::npos)
	{
		info->is_android = true;
		info->identification_reason = "Device/Product name matches Android device";
		return true;
	}

	return false;
}

static void classify_usb_speed(double speed_mbps, std::string* out_standard, std::string* out_tier, std::string* out_throughput)
{
	if (speed_mbps >= 40000.0)
	{
		*out_standard = "USB 4";
		*out_tier = "USB4 Gen 3x2";
		*out_throughput = "40 Gbps";
	}
	else if (speed_mbps >= 20000.0)
	{
		*out_standard = "USB 3 (USB 3.2 Gen 2x2)";
		*out_tier = "SuperSpeed+ (Dual-lane)";
		*out_throughput = "20 Gbps";
	}
	else if (speed_mbps >= 10000.0)
	{
		*out_standard = "USB 3 (USB 3.1 Gen 2 / USB 3.2 Gen 2)";
		*out_tier = "SuperSpeed+";
		*out_throughput = "10 Gbps";
	}
	else if (speed_mbps >= 5000.0)
	{
		*out_standard = "USB 3 (USB 3.0 / USB 3.2 Gen 1)";
		*out_tier = "SuperSpeed";
		*out_throughput = "5 Gbps";
	}
	else if (speed_mbps >= 480.0)
	{
		*out_standard = "USB 2 (USB 2.0)";
		*out_tier = "High-Speed";
		*out_throughput = "480 Mbps (~35-40 MB/s practical)";
	}
	else if (speed_mbps >= 12.0)
	{
		*out_standard = "USB 1 (USB 1.1)";
		*out_tier = "Full-Speed";
		*out_throughput = "12 Mbps (~1.5 MB/s)";
	}
	else
	{
		*out_standard = "USB 1 (USB 1.0)";
		*out_tier = "Low-Speed";
		*out_throughput = "1.5 Mbps";
	}
}

static std::vector<usb_device_info> scan_usb_devices()
{
	std::vector<usb_device_info> devices;
	const char* usb_base = "/sys/bus/usb/devices";
	DIR* dir = opendir(usb_base);
	if (dir == nullptr)
	{
		fprintf(stderr, "Error: Unable to open %s. Is this a Linux system with sysfs mounted?\n", usb_base);
		return devices;
	}

	struct dirent* entry = nullptr;
	while ((entry = readdir(dir)) != nullptr)
	{
		if (entry->d_name[0] == '.')
		{
			continue;
		}
		if (strchr(entry->d_name, ':') != nullptr)
		{
			continue;
		}
		// Skip root hubs (usb1, usb2, etc.)
		if (strncmp(entry->d_name, "usb", 3) == 0)
		{
			continue;
		}

		std::string dev_path = std::string(usb_base) + "/" + entry->d_name;
		std::string id_vendor = read_sysfs_attr(dev_path, "idVendor");
		std::string id_product = read_sysfs_attr(dev_path, "idProduct");
		if (id_vendor.empty() || id_product.empty())
		{
			continue;
		}

		usb_device_info info;
		info.sysfs_path = dev_path;
		info.port_name = entry->d_name;
		info.id_vendor = id_vendor;
		info.id_product = id_product;
		info.manufacturer = read_sysfs_attr(dev_path, "manufacturer");
		info.product = read_sysfs_attr(dev_path, "product");
		info.serial = read_sysfs_attr(dev_path, "serial");
		info.version = read_sysfs_attr(dev_path, "version");
		info.speed_str = read_sysfs_attr(dev_path, "speed");

		std::string bus_str = read_sysfs_attr(dev_path, "busnum");
		std::string dev_str = read_sysfs_attr(dev_path, "devnum");
		info.busnum = bus_str.empty() ? 0 : atoi(bus_str.c_str());
		info.devnum = dev_str.empty() ? 0 : atoi(dev_str.c_str());

		info.speed_mbps = info.speed_str.empty() ? 0.0 : atof(info.speed_str.c_str());
		info.has_adb_interface = check_adb_interface(dev_path);
		info.host_port_max_speed = detect_host_port_max_speed(dev_path);

		evaluate_android_device(&info);

		devices.push_back(info);
	}
	closedir(dir);
	return devices;
}

int main(int argc, char** argv)
{
	(void)argc;
	(void)argv;

	printf("=================================================================\n");
	printf("       Android USB Connection & Speed Diagnostic Tool            \n");
	printf("=================================================================\n\n");

	std::vector<usb_device_info> all_devices = scan_usb_devices();
	std::vector<usb_device_info> android_devices;

	for (size_t i = 0; i < all_devices.size(); i++)
	{
		if (all_devices[i].is_android)
		{
			android_devices.push_back(all_devices[i]);
		}
	}

	if (android_devices.empty())
	{
		printf("No Android devices detected on USB ports.\n");
		if (!all_devices.empty())
		{
			printf("\nOther connected USB devices found:\n");
			for (size_t i = 0; i < all_devices.size(); i++)
			{
				printf("  * [%4s:%4s] %s %s (Speed: %s Mbps)\n",
				       all_devices[i].id_vendor.c_str(),
				       all_devices[i].id_product.c_str(),
				       all_devices[i].manufacturer.c_str(),
				       all_devices[i].product.c_str(),
				       all_devices[i].speed_str.c_str());
			}
		}
		printf("\nPlease verify that USB Debugging is enabled and the phone is plugged in.\n");
		return 0;
	}

	printf("Found %zu connected Android device(s):\n\n", android_devices.size());

	for (size_t i = 0; i < android_devices.size(); i++)
	{
		const usb_device_info& dev = android_devices[i];
		std::string standard;
		std::string tier;
		std::string throughput;
		classify_usb_speed(dev.speed_mbps, &standard, &tier, &throughput);

		printf("-----------------------------------------------------------------\n");
		printf("Device #%zu: %s %s\n", i + 1, dev.manufacturer.c_str(), dev.product.c_str());
		printf("-----------------------------------------------------------------\n");
		printf("  * USB Hardware ID   : VID 0x%s, PID 0x%s\n", dev.id_vendor.c_str(), dev.id_product.c_str());
		if (!dev.serial.empty())
		{
			printf("  * Serial Number     : %s\n", dev.serial.c_str());
		}
		printf("  * Location          : Bus %03d, Device %03d (Port: %s)\n", dev.busnum, dev.devnum, dev.port_name.c_str());
		printf("  * Identification    : %s\n", dev.identification_reason.c_str());
		printf("  * ADB Interface     : %s\n", dev.has_adb_interface ? "ACTIVE" : "Not active / unconfigured");
		printf("  * Device Spec (bcd) : USB %s\n", dev.version.c_str());
		printf("\n");
		printf("  >>> USB STANDARD    : %s <<<\n", standard.c_str());
		printf("  >>> NEGOTIATED SPEED: %s (%s) <<<\n", throughput.c_str(), tier.c_str());
		printf("\n");

		if (dev.speed_mbps < 5000.0)
		{
			printf("  [!] Performance Assessment:\n");
			printf("      The connection is currently limited to USB 2.0 (High-Speed).\n");
			printf("      Max practical transfer speed is capped around ~35-40 MB/s.\n");

			if (!dev.host_port_max_speed.empty() && atof(dev.host_port_max_speed.c_str()) >= 5000.0)
			{
				printf("\n  [!] Diagnostic Hint:\n");
				printf("      Your host USB port supports up to %s Mbps (USB 3 SuperSpeed) via its\n", dev.host_port_max_speed.c_str());
				printf("      companion controller, but the link negotiated at only %s Mbps.\n", dev.speed_str.c_str());
				printf("      This almost always indicates the USB cable only has USB 2.0 data pins\n");
				printf("      (e.g., standard phone in-box charging cables) or an intermediate hub is USB 2.0.\n");
				printf("      Switching to a certified USB 3.0+ / USB-C SuperSpeed cable will unlock Gbps+ speeds.\n");
			}
		}
		else
		{
			printf("  [+] Performance Assessment:\n");
			printf("      SuperSpeed connection active! Ready for high-throughput raw USB\n");
			printf("      and low-latency trace/data streaming.\n");
		}
		printf("-----------------------------------------------------------------\n\n");
	}

	return 0;
}
