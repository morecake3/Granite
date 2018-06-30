/* Copyright (c) 2017-2018 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

// Loosely copied from RetroArch's udev implementation.

#include "input_linux.hpp"
#include "unstable_remove_if.hpp"
#include "util.hpp"
#include <stdexcept>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/kd.h>
#include <poll.h>
#include <string.h>
#include <signal.h>
#include <termio.h>

using namespace std;

namespace Granite
{
static long old_kbmd = 0xffff;
static struct termios old_term;

static void terminal_flush()
{
	tcsetattr(0, TCSAFLUSH, &old_term);
}

static void terminal_enable_input()
{
	if (old_kbmd == 0xffff)
		return;

	if (ioctl(0, KDSKBMODE, old_kbmd) < 0)
		return;

	terminal_flush();
	old_kbmd = 0xffff;
}

static void terminal_restore_signal(int sig)
{
	terminal_enable_input();
	kill(getpid(), sig);
}

static bool terminal_disable_input()
{
	if (!isatty(0))
		return false;

	if (old_kbmd != 0xffff)
		return false;

	if (tcgetattr(0, &old_term) < 0)
		return false;

	auto new_term = old_term;
	new_term.c_lflag &= ~(ECHO | ICANON | ISIG);
	new_term.c_iflag &= ~(ISTRIP | IGNCR | ICRNL | INLCR | IXOFF | IXON);
	new_term.c_cc[VMIN] = 0;
	new_term.c_cc[VTIME] = 0;

	if (ioctl(0, KDGKBMODE, &old_kbmd) < 0)
		return false;

	if (tcsetattr(0, TCSAFLUSH, &new_term) < 0)
		return false;

	if (ioctl(0, KDSKBMODE, K_MEDIUMRAW) < 0)
	{
		terminal_flush();
		return false;
	}

	struct sigaction sa = {};
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART | SA_RESETHAND;
	sa.sa_handler = terminal_restore_signal;

	sigaction(SIGABRT, &sa, nullptr);
	sigaction(SIGBUS, &sa, nullptr);
	sigaction(SIGFPE, &sa, nullptr);
	sigaction(SIGILL, &sa, nullptr);
	sigaction(SIGQUIT, &sa, nullptr);
	sigaction(SIGSEGV, &sa, nullptr);

	atexit(terminal_enable_input);
	return true;
}

const char *LinuxInputManager::get_device_type_string(DeviceType type)
{
	switch (type)
	{
	case DeviceType::Keyboard:
		return "ID_INPUT_KEYBOARD";
	case DeviceType::Mouse:
		return "ID_INPUT_MOUSE";
	case DeviceType::Touchpad:
		return "ID_INPUT_TOUCHPAD";
	case DeviceType::Joystick:
		return "ID_INPUT_JOYSTICK";
	}
	return nullptr;
}

bool LinuxInputManager::add_device(int fd, DeviceType type, const char *devnode, InputCallback callback)
{
	struct stat s;
	if (fstat(fd, &s) < 0)
		return false;

	epoll_event event;
	auto dev = make_unique<Device>();
	dev->type = type;
	dev->fd = fd;
	dev->callback = callback;
	dev->devnode = devnode;

	event.data.ptr = dev.get();
	event.events = EPOLLIN;

	if (epoll_ctl(queue_fd, EPOLL_CTL_ADD, fd, &event) < 0)
	{
		LOGE("Failed to add FD to epoll.\n");
		return false;
	}

	devices.push_back(move(dev));
	return true;
}

bool LinuxInputManager::hotplug_available()
{
	struct pollfd fds = {};
	fds.fd = udev_monitor_get_fd(udev_monitor);
	fds.events = POLLIN;

	return (::poll(&fds, 1, 0) == 1) && (fds.revents & POLLIN);
}

void LinuxInputManager::handle_hotplug()
{
	auto *dev = udev_monitor_receive_device(udev_monitor);

	auto *val_key = udev_device_get_property_value(dev, "ID_INPUT_KEYBOARD");
	auto *val_mouse = udev_device_get_property_value(dev, "ID_INPUT_MOUSE");
	auto *val_touchpad = udev_device_get_property_value(dev, "ID_INPUT_TOUCHPAD");
	auto *val_joystick = udev_device_get_property_value(dev, "ID_INPUT_JOYSTICK");
	auto *action = udev_device_get_action(dev);
	auto *devnode = udev_device_get_devnode(dev);

	InputCallback cb = nullptr;
	DeviceType type;
	if (val_key && strcmp(val_key, "1") == 0 && devnode)
	{
		type = DeviceType::Keyboard;
		cb = &LinuxInputManager::input_handle_keyboard;
	}
	else if (val_mouse && strcmp(val_mouse, "1") == 0 && devnode)
	{
		type = DeviceType::Mouse;
		cb = &LinuxInputManager::input_handle_mouse;
	}
	else if (val_touchpad && strcmp(val_touchpad, "1") == 0 && devnode)
	{
		type = DeviceType::Touchpad;
		cb = &LinuxInputManager::input_handle_touchpad;
	}
	else if (val_joystick && strcmp(val_joystick, "1") == 0 && devnode)
	{
		type = DeviceType::Joystick;
		cb = &LinuxInputManager::input_handle_joystick;
	}
	else
		return;

	if (strcmp(action, "add") == 0)
	{
		LOGI("Hotplugging %s\n", devnode);
		int fd = open(devnode, O_RDONLY | O_NONBLOCK);
		if (fd >= 0)
		{
			if (!add_device(fd, type, devnode, cb))
			{
				LOGE("Failed to hotplug: %s\n", devnode);
				close(fd);
			}
		}
		else
			LOGE("Failed to open device: %s.\n", devnode);
	}
	else if (strcmp(action, "remove") == 0)
	{
		remove_device(devnode);
	}

	udev_device_unref(dev);
}

bool LinuxInputManager::poll(InputTracker &tracker)
{
	if (queue_fd < 0)
		return false;

	while (hotplug_available())
		handle_hotplug();

	int ret;
	epoll_event events[32];
	while ((ret = epoll_wait(queue_fd, events, 32, 0)) > 0)
	{
		for (int i = 0; i < ret; i++)
		{
			if (events[i].events & EPOLLIN)
			{
				struct input_event input_events[32];
				auto &device = *static_cast<Device *>(events[i].data.ptr);

				ssize_t len;
				while ((len = read(device.fd, input_events, 32)) > 0)
				{
					len /= sizeof(input_events[0]);
					for (ssize_t j = 0; j < len; j++)
						(this->*device.callback)(tracker, device, input_events[j]);
				}
			}
		}
	}

	return true;
}

void LinuxInputManager::remove_device(const char *devnode)
{
	auto itr = Util::unstable_remove_if(begin(devices), end(devices), [=](const unique_ptr<Device> &dev) {
		return dev->devnode == devnode;
	});

	devices.erase(itr, end(devices));
}

bool LinuxInputManager::open_devices(DeviceType type, InputCallback callback)
{
	const char *type_str = get_device_type_string(type);
	udev_enumerate *enumerate = udev_enumerate_new(udev);
	if (!enumerate)
		return false;

	udev_enumerate_add_match_property(enumerate, type_str, "1");
	udev_enumerate_scan_devices(enumerate);
	udev_list_entry *devs = udev_enumerate_get_list_entry(enumerate);

	for (auto *item = devs; item != nullptr; item = udev_list_entry_get_next(item))
	{
		auto *name = udev_list_entry_get_name(item);
		auto *dev = udev_device_new_from_syspath(udev, name);
		auto *devnode = udev_device_get_devnode(dev);

		if (devnode)
		{
			int fd = open(devnode, O_RDONLY | O_NONBLOCK);
			if (fd != -1)
			{
				if (!add_device(fd, type, devnode, callback))
				{
					close(fd);
					LOGE("Failed to add device: %s\n", devnode);
				}
				else
					LOGI("Found %s (%s)\n", type_str, devnode);
			}
		}

		udev_device_unref(dev);
	}

	udev_enumerate_unref(enumerate);
	return true;
}

void LinuxInputManager::init_key_table()
{
	for (auto &key : keyboard_to_key)
		key = Key::Unknown;

#define set_key(key) keyboard_to_key[KEY_##key] = Key::key
	set_key(A);
	set_key(B);
	set_key(C);
	set_key(D);
	set_key(E);
	set_key(F);
	set_key(G);
	set_key(H);
	set_key(I);
	set_key(J);
	set_key(K);
	set_key(L);
	set_key(M);
	set_key(N);
	set_key(O);
	set_key(P);
	set_key(Q);
	set_key(R);
	set_key(S);
	set_key(T);
	set_key(U);
	set_key(V);
	set_key(W);
	set_key(X);
	set_key(Y);
	set_key(Z);
	keyboard_to_key[KEY_ESC] = Key::Escape;
	keyboard_to_key[KEY_ENTER] = Key::Return;
	keyboard_to_key[KEY_SPACE] = Key::Space;
	keyboard_to_key[KEY_LEFTALT] = Key::LeftAlt;
	keyboard_to_key[KEY_LEFTCTRL] = Key::LeftCtrl;
	keyboard_to_key[KEY_LEFTSHIFT] = Key::LeftShift;
#undef set_key
}

void LinuxInputManager::input_handle_keyboard(InputTracker &tracker, Device &, const input_event &e)
{
	switch (e.type)
	{
	case EV_KEY:
	{
		bool pressed = e.value != 0;
		Key key = Key::Unknown;
		if (e.code < KEY_MAX)
			key = keyboard_to_key[e.code];

		if (key != Key::Unknown)
			tracker.key_event(key, pressed ? KeyState::Pressed : KeyState::Released);
		break;
	}

	default:
		break;
	}
}

void LinuxInputManager::input_handle_mouse(InputTracker &tracker, Device &, const input_event &e)
{
	switch (e.type)
	{
	case EV_KEY:
		switch (e.code)
		{
		case BTN_LEFT:
			tracker.mouse_button_event(MouseButton::Left,
			                           e.value != 0);
			break;

		case BTN_RIGHT:
			tracker.mouse_button_event(MouseButton::Right,
			                           e.value != 0);
			break;

		case BTN_MIDDLE:
			tracker.mouse_button_event(MouseButton::Middle,
			                           e.value != 0);
			break;

		default:
			break;
		}
		break;

	case EV_REL:
		switch (e.code)
		{
		case REL_X:
			tracker.mouse_move_event_relative(double(e.value), 0.0);
			break;

		case REL_Y:
			tracker.mouse_move_event_relative(0.0, double(e.value));
			break;

		default:
			break;
		}
		break;

	default:
		break;
	}
}

void LinuxInputManager::input_handle_touchpad(InputTracker &, Device &, const input_event &)
{
}

void LinuxInputManager::input_handle_joystick(InputTracker &, Device &, const input_event &)
{
}

bool LinuxInputManager::init()
{
	terminal_disable_input();
	init_key_table();

	udev = udev_new();
	if (!udev)
	{
		LOGE("Failed to create UDEV.\n");
		return false;
	}

	udev_monitor = udev_monitor_new_from_netlink(udev, "udev");
	if (!udev_monitor)
	{
		LOGE("Failed to create UDEV monitor.\n");
		return false;
	}

	udev_monitor_filter_add_match_subsystem_devtype(udev_monitor, "input", nullptr);
	udev_monitor_enable_receiving(udev_monitor);

	queue_fd = epoll_create(32);
	if (queue_fd < 0)
	{
		LOGE("Failed to create epoll FD.\n");
		return false;
	}

	if (!open_devices(DeviceType::Keyboard, &LinuxInputManager::input_handle_keyboard))
	{
		LOGE("Failed to open keyboards.\n");
		return false;
	}

	if (!open_devices(DeviceType::Mouse, &LinuxInputManager::input_handle_mouse))
	{
		LOGE("Failed to open keyboards.\n");
		return false;
	}

	if (!open_devices(DeviceType::Touchpad, &LinuxInputManager::input_handle_touchpad))
	{
		LOGE("Failed to open keyboards.\n");
		return false;
	}

	if (!open_devices(DeviceType::Joystick, &LinuxInputManager::input_handle_joystick))
	{
		LOGE("Failed to open joysticks.\n");
		return false;
	}

	return true;
}

LinuxInputManager::~LinuxInputManager()
{
	if (udev_monitor)
		udev_monitor_unref(udev_monitor);
	if (udev)
		udev_unref(udev);
	if (queue_fd >= 0)
		close(queue_fd);
}

LinuxInputManager::Device::~Device()
{
	if (fd >= 0)
		close(fd);
}

}
