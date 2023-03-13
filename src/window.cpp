#include "window.h"
#include "util.h"

#include <string>
#include <cstring>

#if VK_USE_PLATFORM_ANDROID_KHR
#include "android_utils.h"
#endif
#ifdef VK_USE_PLATFORM_XCB_KHR
#include <xcb/xcb.h>
#include <xcb/randr.h>
#endif
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
#include <wayland-client.h>
#endif

struct LWindow
{
	std::string winsys;
#ifdef VK_USE_PLATFORM_XCB_KHR
	struct
	{
		xcb_connection_t* connection = nullptr;
		xcb_screen_t* screen = nullptr;
		xcb_window_t window = 0;
		xcb_atom_t protocol_atom;
		xcb_atom_t delete_window_atom;
		xcb_atom_t state_atom;
		xcb_atom_t state_fullscreen_atom;
		xcb_atom_t bypass_compositor_atom;
	} xcb;
#endif
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
	struct
	{
		struct wl_display *display = nullptr;
		struct wl_registry *registry = nullptr;
		struct wl_compositor *compositor = nullptr;
		struct wl_shell *shell = nullptr;
		struct wl_surface *wl_surface = nullptr;
		struct wl_shell_surface *shell_surface = nullptr;
		struct wl_seat *seat = nullptr;
		struct wl_pointer *pointer = nullptr;
		struct wl_keyboard *keyboard = nullptr;
		struct wl_output *output = nullptr;
	} wayland;
#endif
#if VK_USE_PLATFORM_ANDROID_KHR
	struct
	{
		ANativeWindow *window = nullptr;
	} android;
#endif
	uint32_t index;
	int32_t x;
	int32_t y;
	int32_t width;
	int32_t height;
	bool fullscreen = false;
};
static std::vector<LWindow> index_to_LWindow;
static std::string override_winsys;

void window_preallocate(uint32_t size)
{
	index_to_LWindow.resize(size);
}

void window_set_winsys(const std::string& name)
{
	override_winsys = name;
}

const char* window_winsys()
{
	if (!override_winsys.empty())
	{
		return override_winsys.c_str();
	}

#ifdef VK_USE_PLATFORM_ANDROID_KHR
	return "android";
#else
	const char* winsys = getenv("LAVATUBE_WINSYS");
	if (!winsys)
	{
#ifdef VK_USE_PLATFORM_XCB_KHR
		return "xcb";
#elif VK_USE_PLATFORM_WAYLAND_KHR
		return "wayland";
#else
		return "headless";
#endif
	}

	return winsys;
#endif
}

#ifdef VK_USE_PLATFORM_XCB_KHR
static const char* lavaxcb_strerror(int err)
{
	if (err == XCB_CONN_ERROR) return "Connection error";
	else if (err == XCB_CONN_CLOSED_EXT_NOTSUPPORTED) return "Extension not supported";
	else if (err == XCB_CONN_CLOSED_MEM_INSUFFICIENT) return "Insufficient memory";
	else if (err == XCB_CONN_CLOSED_REQ_LEN_EXCEED) return "Request length exceeded";
	else if (err == XCB_CONN_CLOSED_PARSE_ERR) return "Parse error";
	else if (err == XCB_CONN_CLOSED_INVALID_SCREEN) return "Invalid screen";
	else if (err == XCB_CONN_CLOSED_FDPASSING_FAILED) return "Invalid file descriptor";
	else return "Unknown error";
}

static xcb_intern_atom_cookie_t lavaxcb_send_atom_request(xcb_connection_t* connection, const char* name, uint8_t only_if_exists)
{
	return xcb_intern_atom(connection, only_if_exists, strlen(name), name);
}

static xcb_atom_t lavaxcb_get_atom_reply(xcb_connection_t* connection, const char* name, xcb_intern_atom_cookie_t cookie)
{
	xcb_atom_t atom  = 0;
	xcb_generic_error_t* error = nullptr;
	xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(connection, cookie, &error);

	if (reply != nullptr)
	{
		atom = reply->atom;
		free(reply);
	}
	else
	{
		ELOG("Failed to retrieve internal XCB atom for %s with error %u", name, error->error_code);
		free(error);
	}

	return atom;
}

// TBD fix race condition for multi-window; also message could be something we don't want to miss or for other window
static bool lavaxcb_wait(xcb_connection_t* connection, uint32_t type)
{
	do
	{
		const xcb_generic_event_t* event = xcb_poll_for_event(connection);
		if (event == nullptr) return false;
		if (event->response_type == 0) return false;
		const uint8_t event_code = event->response_type & 0x7f;
		if (event_code == XCB_DESTROY_NOTIFY) return false;
		if (event_code == type) return true;
		usleep(1);
	} while (true);
	return false;
}
#endif

VkSurfaceKHR window_create(VkInstance instance, uint32_t index, int32_t x, int32_t y, int32_t width, int32_t height)
{
	DLOG2("asked to create window %u of size %d,%d at pos %d,%d", index, width, height, x, y);
	VkSurfaceKHR surface = 0;
	LWindow& w = index_to_LWindow.at(index);
	w.winsys = window_winsys();
	w.x = x;
	w.y = y;
	w.width = width;
	w.height = height;
	if (w.winsys == "android")
	{
#if VK_USE_PLATFORM_ANDROID_KHR
		w.android.window = AndroidGlobs::G_STATE->pendingWindow;
		if (AndroidGlobs::G_STATE == nullptr)
		{
			ABORT("Global android state not initialized");
		} else if (AndroidGlobs::G_STATE->pendingWindow == nullptr) {
			ABORT("Android window not initialized");
		}

		VkAndroidSurfaceCreateInfoKHR pInfo = {};
		pInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
		pInfo.pNext = nullptr;
		pInfo.flags = 0;
		pInfo.window = w.android.window;

		VkResult result = wrap_vkCreateAndroidSurfaceKHR(instance, &pInfo, nullptr, &surface);
		if (result != VK_SUCCESS)
		{
			ABORT("Failed to create Android Vulkan surface");
		}
#else
		ABORT("Android surfaces not supported");
#endif
	}
	else if (w.winsys == "xcb")
	{
#ifndef VK_USE_PLATFORM_XCB_KHR
		ABORT("XCB surfaces not supported");
#else
		int scr = 0;
		w.xcb.connection = xcb_connect(nullptr, &scr);
		int err = xcb_connection_has_error(w.xcb.connection);
		if (err)
		{
			ABORT("Failed to connect to XCB server: %s (%d)", lavaxcb_strerror(err), err);
		}
		const xcb_setup_t *setup = xcb_get_setup(w.xcb.connection);
		if (!setup) ABORT("Failed to setup XCB connection");
		xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
		while (scr-- > 0) xcb_screen_next(&iter);
		w.xcb.screen = iter.data;
		uint32_t value_mask, value_list[32];
		value_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
		value_list[0] = w.xcb.screen->black_pixel;
		value_list[1] = XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_EXPOSURE;
		xcb_generic_error_t *error = nullptr;
		w.xcb.window = xcb_generate_id(w.xcb.connection);

		// Get screen dimensions
		xcb_get_geometry_cookie_t geomcookie = xcb_get_geometry(w.xcb.connection, w.xcb.screen->root);
		xcb_get_geometry_reply_t *geomreply;
		bool fullscreen = false;
		if ((geomreply = xcb_get_geometry_reply(w.xcb.connection, geomcookie, &error)))
		{
			DLOG("XCB screen size is %d x %d, window is %d x %d, with border=%d\n", geomreply->width, geomreply->height, width, height, (int)geomreply->border_width);
			if (geomreply->width == width || geomreply->height == height) // go fullscreen?
			{
				x = 0;
				y = 0;
				fullscreen = true;
			}
			else if (geomreply->width < width || geomreply->height < height) // warn if screen is too small
			{
				ELOG("Screen is smaller than window - this may be a problem for replay.");
			}
			free(geomreply);
		}
		else
		{
			ELOG("Failed to get screen geometry: %u", error->error_code);
			free(error);
		}

		xcb_void_cookie_t cookie = xcb_create_window_checked(w.xcb.connection, XCB_COPY_FROM_PARENT, w.xcb.window, w.xcb.screen->root, 0, 0, width, height, 0,
			XCB_WINDOW_CLASS_INPUT_OUTPUT, w.xcb.screen->root_visual, value_mask, value_list);
		if ((error = xcb_request_check(w.xcb.connection, cookie)))
		{
			ABORT("Failed to create XCB window (w=%u, h=%u): %s (%d)", (unsigned)width, (unsigned)height, lavaxcb_strerror(error->error_code), error->error_code);
			free(error);
		}
		cookie = xcb_map_window_checked(w.xcb.connection, w.xcb.window);
		if ((error = xcb_request_check(w.xcb.connection, cookie)))
		{
			ABORT("Failed to show XCB window: %u", error->error_code);
			free(error);
		}

		// grab us some atoms
		xcb_intern_atom_cookie_t protocol_atom_cookie = lavaxcb_send_atom_request(w.xcb.connection, "WM_PROTOCOLS", 1);
		xcb_intern_atom_cookie_t delete_window_atom_cookie = lavaxcb_send_atom_request(w.xcb.connection, "WM_DELETE_WINDOW", 0);
		xcb_intern_atom_cookie_t state_atom_cookie = lavaxcb_send_atom_request(w.xcb.connection, "_NET_WM_STATE", 1);
		xcb_intern_atom_cookie_t state_fullscreen_atom_cookie = lavaxcb_send_atom_request(w.xcb.connection, "_NET_WM_STATE_FULLSCREEN", 0);
		xcb_intern_atom_cookie_t bypass_compositor_atom_cookie = lavaxcb_send_atom_request(w.xcb.connection, "_NET_WM_BYPASS_COMPOSITOR", 0);
		w.xcb.protocol_atom = lavaxcb_get_atom_reply(w.xcb.connection, "WM_PROTOCOLS", protocol_atom_cookie);
		w.xcb.delete_window_atom = lavaxcb_get_atom_reply(w.xcb.connection, "WM_DELETE_WINDOW", delete_window_atom_cookie);
		w.xcb.state_atom = lavaxcb_get_atom_reply(w.xcb.connection, "_NET_WM_STATE", state_atom_cookie);
		w.xcb.state_fullscreen_atom = lavaxcb_get_atom_reply(w.xcb.connection, "_NET_WM_STATE_FULLSCREEN", state_fullscreen_atom_cookie);
		w.xcb.bypass_compositor_atom = lavaxcb_get_atom_reply(w.xcb.connection, "_NET_WM_BYPASS_COMPOSITOR", bypass_compositor_atom_cookie);

		xcb_flush(w.xcb.connection);
		VkXcbSurfaceCreateInfoKHR pInfo = {};
		pInfo.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
		pInfo.flags = 0;
		pInfo.connection = w.xcb.connection;
		pInfo.window = w.xcb.window;
		VkResult result = wrap_vkCreateXcbSurfaceKHR(instance, &pInfo, nullptr, &surface);
		if (result != VK_SUCCESS)
		{
			ABORT("Failed to create XCB Vulkan surface");
		}

		window_fullscreen(index, fullscreen);
#endif
	}
	else if (w.winsys == "x11")
	{
		ABORT("XLIB surfaces not supported");
	}
	else if (w.winsys == "wayland")
	{
#ifndef VK_USE_PLATFORM_WAYLAND_KHR
		ABORT("Wayland surfaces not supported");
#else
		w.wayland.display = wl_display_connect(nullptr);
		if (!w.wayland.display) ABORT("Failed to connect to Wayland display server");
		w.wayland.registry = wl_display_get_registry(m_display);
		if (!w.wayland.registry) ABORT("Failed to connect to Wayland registry");
		//wl_registry_add_listener(w.wayland.registry, &vkDisplayWayland::registry_listener, this);
		wl_display_roundtrip(w.wayland.display);
		w.wayland.surface = wl_compositor_create_surface(w.wayland.compositor);
		w.wayland.
		VkWaylandSurfaceCreateInfoKHR pInfo = {};
		pInfo.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
		pInfo.pNext = nullptr;
		pInfo.flags = 0;
		pInfo.display = (wl_display*)m_context_descriptor.m_user_data[0];
		pInfo.surface = (wl_surface*)m_context_descriptor.m_user_data[1];
#endif
	}
	else if (w.winsys == "headless")
	{
		VkHeadlessSurfaceCreateInfoEXT pInfo = {};
		pInfo.pNext = nullptr;
		pInfo.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
		pInfo.flags = 0;
		PFN_vkCreateHeadlessSurfaceEXT hack_vkCreateHeadlessSurfaceEXT = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(wrap_vkGetInstanceProcAddr(instance,"vkCreateHeadlessSurfaceEXT"));
		VkResult result = hack_vkCreateHeadlessSurfaceEXT(instance, &pInfo, nullptr, &surface);
		if (result != VK_SUCCESS)
		{
			ABORT("Failed to create headless surface");
		}
	}

	(void)instance;
	return surface;
}

void window_fullscreen(uint32_t index, bool value)
{
#ifdef VK_USE_PLATFORM_XCB_KHR
	LWindow& w = index_to_LWindow.at(index);
	if (value != w.fullscreen)
	{
		if (value) ILOG("Entering fullscreen mode");
		xcb_client_message_event_t event;
		event.response_type = XCB_CLIENT_MESSAGE;
		event.format = 32;
		event.sequence = 0;
		event.window = w.xcb.window;
		event.type = w.xcb.state_atom;
		event.data.data32[0] = value ? 1 : 0;
		event.data.data32[1] = w.xcb.state_fullscreen_atom;
		event.data.data32[2] = 0;
		event.data.data32[3] = 0;
		event.data.data32[4] = 0;
		xcb_send_event(w.xcb.connection, 0, w.xcb.screen->root, XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT, (const char*)&event);
		xcb_flush(w.xcb.connection);
		if (lavaxcb_wait(w.xcb.connection, XCB_CONFIGURE_NOTIFY))
		{
			w.fullscreen = value;
			const int32_t bypass = value ? 2 : 0; // bypass compositor to workaround VK_ERROR_OUT_OF_DATE_KHR on GNOME + NVIDIA
			xcb_change_property(w.xcb.connection, XCB_PROP_MODE_REPLACE, w.xcb.window, w.xcb.bypass_compositor_atom, XCB_ATOM_CARDINAL, 32, 1, &bypass);
			xcb_flush(w.xcb.connection);
			usleep(50000); // not sure if needed, but gfxreconstruct does this so why not
		}
		else ELOG("Failed to %s fullscreen mode", value ? "enter" : "leave");
	}
#endif
	(void)index;
	(void)value;
}

void window_destroy(VkInstance instance, uint32_t index)
{
	window_fullscreen(index, false);
#ifdef VK_USE_PLATFORM_XCB_KHR
	LWindow& w = index_to_LWindow.at(index);
	if (w.winsys == "xcb")
	{
		if (w.xcb.window != 0)
		{
			xcb_destroy_window(w.xcb.connection, w.xcb.window);
		}
		if (w.xcb.connection != nullptr)
		{
			xcb_disconnect(w.xcb.connection);
		}
	}
#endif
	(void)instance;
}
