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

#include <thread>
#include "wsi.hpp"

using namespace std;

namespace Vulkan
{
WSI::WSI()
{
}

bool WSI::reinit_external_swapchain(std::vector<Vulkan::ImageHandle> external_images)
{
	if (!init_external_swapchain(move(external_images)))
		return false;
	return true;
}

bool WSI::init_external_context(std::unique_ptr<Vulkan::Context> fresh_context)
{
	context = move(fresh_context);

	// Need to have a dummy swapchain in place before we issue create device events.
	device.reset(new Device);
	device->set_context(*context);
	device->init_external_swapchain({ ImageHandle(nullptr) });
	platform->event_device_created(device.get());
	return true;
}

bool WSI::init_external_swapchain(std::vector<Vulkan::ImageHandle> swapchain_images)
{
	width = platform->get_surface_width();
	height = platform->get_surface_height();
	aspect_ratio = platform->get_aspect_ratio();

	external_swapchain_images = move(swapchain_images);

	this->width = external_swapchain_images.front()->get_width();
	this->height = external_swapchain_images.front()->get_height();
	this->format = external_swapchain_images.front()->get_format();

	LOGI("Created swapchain %u x %u (fmt: %u).\n", this->width, this->height, static_cast<unsigned>(this->format));

	platform->event_swapchain_destroyed();
	platform->event_swapchain_created(device.get(), this->width, this->height, aspect_ratio,
	                                  external_swapchain_images.size(), this->format);

	device->init_external_swapchain(this->external_swapchain_images);
	platform->get_frame_timer().reset();
	external_acquire.reset();
	external_release.reset();
	return true;
}

void WSI::set_platform(WSIPlatform *platform)
{
	this->platform = platform;
}

bool WSI::init()
{
	auto instance_ext = platform->get_instance_extensions();
	auto device_ext = platform->get_device_extensions();
	context.reset(new Context(instance_ext.data(), instance_ext.size(), device_ext.data(), device_ext.size()));

	// Need to have a dummy swapchain in place before we issue create device events.
	device.reset(new Device);
	device->set_context(*context);
	device->init_external_swapchain({ ImageHandle(nullptr) });

	platform->event_device_created(device.get());

	surface = platform->create_surface(context->get_instance(), context->get_gpu());
	if (surface == VK_NULL_HANDLE)
		return false;

	unsigned width = platform->get_surface_width();
	unsigned height = platform->get_surface_height();
	aspect_ratio = platform->get_aspect_ratio();

	VkBool32 supported = VK_FALSE;
	vkGetPhysicalDeviceSurfaceSupportKHR(context->get_gpu(), context->get_graphics_queue_family(), surface, &supported);
	if (!supported)
		return false;

	if (!blocking_init_swapchain(width, height))
		return false;

	device->init_swapchain(swapchain_images, this->width, this->height, format);
	platform->get_frame_timer().reset();
	return true;
}

void WSI::init_surface_and_swapchain(VkSurfaceKHR new_surface)
{
	LOGI("init_surface_and_swapchain()\n");
	if (new_surface != VK_NULL_HANDLE)
	{
		VK_ASSERT(surface == VK_NULL_HANDLE);
		surface = new_surface;
	}

	width = platform->get_surface_width();
	height = platform->get_surface_height();
	update_framebuffer(width, height);
}

void WSI::deinit_surface_and_swapchain()
{
	LOGI("deinit_surface_and_swapchain()\n");
	device->wait_idle();

	device->set_acquire_semaphore(Semaphore{});
	device->consume_release_semaphore();

	if (swapchain != VK_NULL_HANDLE)
		vkDestroySwapchainKHR(context->get_device(), swapchain, nullptr);
	swapchain = VK_NULL_HANDLE;
	need_acquire = true;

	if (surface != VK_NULL_HANDLE)
		vkDestroySurfaceKHR(context->get_instance(), surface, nullptr);
	surface = VK_NULL_HANDLE;

	platform->event_swapchain_destroyed();
}

void WSI::set_external_frame(unsigned index, Vulkan::Semaphore acquire_semaphore, double frame_time)
{
	external_frame_index = index;
	external_acquire = move(acquire_semaphore);
	frame_is_external = true;
	external_frame_time = frame_time;
}

bool WSI::begin_frame_external()
{
	// Need to handle this stuff from outside.
	if (!need_acquire)
		return false;

	auto frame_time = platform->get_frame_timer().frame(external_frame_time);
	auto elapsed_time = platform->get_frame_timer().get_elapsed();

	// Poll after acquire as well for optimal latency.
	platform->poll_input();

	swapchain_index = external_frame_index;
	platform->event_frame_tick(frame_time, elapsed_time);

	device->begin_frame(swapchain_index);
	platform->event_swapchain_index(device.get(), swapchain_index);

	if (external_acquire)
		device->set_acquire_semaphore(external_acquire);
	else
		device->set_acquire_semaphore(Semaphore{});

	external_acquire.reset();
	return true;
}

Semaphore WSI::consume_external_release_semaphore()
{
	Semaphore sem;
	swap(external_release, sem);
	return sem;
}

bool WSI::begin_frame()
{
	if (frame_is_external)
		return begin_frame_external();

	if (swapchain == VK_NULL_HANDLE || platform->should_resize())
	{
		update_framebuffer(platform->get_surface_width(), platform->get_surface_height());
		platform->acknowledge_resize();
	}

	if (swapchain == VK_NULL_HANDLE)
	{
		LOGE("Completely lost swapchain. Cannot continue.\n");
		return false;
	}

	if (!need_acquire)
		return true;

	external_release.reset();

	VkResult result;
	do
	{
		auto acquire = device->request_semaphore();
		result = vkAcquireNextImageKHR(context->get_device(), swapchain, UINT64_MAX,
		                               acquire->get_semaphore(), VK_NULL_HANDLE,
		                               &swapchain_index);

		if (result == VK_SUCCESS)
		{
			acquire->signal_external();

			auto frame_time = platform->get_frame_timer().frame();
			auto elapsed_time = platform->get_frame_timer().get_elapsed();

			// Poll after acquire as well for optimal latency.
			platform->poll_input();
			platform->event_frame_tick(frame_time, elapsed_time);

			device->begin_frame(swapchain_index);
			platform->event_swapchain_index(device.get(), swapchain_index);
			device->set_acquire_semaphore(acquire);
		}
		else if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR)
		{
			VK_ASSERT(width != 0);
			VK_ASSERT(height != 0);
			vkDeviceWaitIdle(device->get_device());

			if (swapchain != VK_NULL_HANDLE)
			{
				vkDestroySwapchainKHR(device->get_device(), swapchain, nullptr);
				swapchain = VK_NULL_HANDLE;
			}

			device->set_acquire_semaphore(Semaphore{});
			device->consume_release_semaphore();

			if (!blocking_init_swapchain(width, height))
				return false;
			device->init_swapchain(swapchain_images, this->width, this->height, format);
		}
		else
		{
			return false;
		}
	} while (result != VK_SUCCESS);
	return true;
}

bool WSI::end_frame()
{
	device->end_frame();

	if (!device->swapchain_touched())
	{
		device->wait_idle();
		return true;
	}

	need_acquire = true;

	// Take ownership of the release semaphore so that the external user can use it.
	if (frame_is_external)
	{
		external_release = device->consume_release_semaphore();
		VK_ASSERT(external_release);
		VK_ASSERT(external_release->is_signalled());
		frame_is_external = false;
	}
	else
	{
		auto release = device->consume_release_semaphore();
		VK_ASSERT(release);
		VK_ASSERT(release->is_signalled());
		auto release_semaphore = release->consume();
		VK_ASSERT(release_semaphore != VK_NULL_HANDLE);

		VkResult result = VK_SUCCESS;
		VkPresentInfoKHR info = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
		info.waitSemaphoreCount = 1;
		info.pWaitSemaphores = &release_semaphore;
		info.swapchainCount = 1;
		info.pSwapchains = &swapchain;
		info.pImageIndices = &swapchain_index;
		info.pResults = &result;

		VkResult overall = vkQueuePresentKHR(context->get_graphics_queue(), &info);
		if (overall != VK_SUCCESS || result != VK_SUCCESS)
		{
			LOGE("vkQueuePresentKHR failed.\n");
			device->wait_idle();
			vkDestroySemaphore(device->get_device(), release_semaphore, nullptr);
			vkDestroySwapchainKHR(device->get_device(), swapchain, nullptr);
			swapchain = VK_NULL_HANDLE;
			return false;
		}
		else
		{
			if (release->can_recycle())
				device->frame().recycled_semaphores.push_back(release_semaphore);
			else
				device->frame().destroyed_semaphores.push_back(release_semaphore);
		}
	}

	return true;
}

void WSI::update_framebuffer(unsigned width, unsigned height)
{
	vkDeviceWaitIdle(context->get_device());

	aspect_ratio = platform->get_aspect_ratio();
	if (blocking_init_swapchain(width, height))
		device->init_swapchain(swapchain_images, this->width, this->height, format);
}

void WSI::deinit_external()
{
	if (platform)
		platform->release_resources();

	if (context)
	{
		vkDeviceWaitIdle(context->get_device());

		device->set_acquire_semaphore(Semaphore{});
		device->consume_release_semaphore();

		platform->event_swapchain_destroyed();
		if (swapchain != VK_NULL_HANDLE)
			vkDestroySwapchainKHR(context->get_device(), swapchain, nullptr);
	}

	if (surface != VK_NULL_HANDLE)
		vkDestroySurfaceKHR(context->get_instance(), surface, nullptr);

	platform->event_device_destroyed();
	external_release.reset();
	external_acquire.reset();
	external_swapchain_images.clear();
	device.reset();
	context.reset();
}

bool WSI::blocking_init_swapchain(unsigned width, unsigned height)
{
	SwapchainError err;
	unsigned retry_counter = 0;
	do
	{
		err = init_swapchain(width, height);
		if (err == SwapchainError::Error)
		{
			if (++retry_counter > 3)
				return false;

			// Try to not reuse the swapchain.
			vkDeviceWaitIdle(device->get_device());
			if (swapchain != VK_NULL_HANDLE)
				vkDestroySwapchainKHR(device->get_device(), swapchain, nullptr);
			swapchain = VK_NULL_HANDLE;
		}
		else if (err == SwapchainError::NoSurface && platform->alive(*this))
		{
			platform->poll_input();
			this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	} while (err != SwapchainError::None);

	return swapchain != VK_NULL_HANDLE;
}

WSI::SwapchainError WSI::init_swapchain(unsigned width, unsigned height)
{
	VkSurfaceCapabilitiesKHR surface_properties;
	auto gpu = context->get_gpu();
	if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &surface_properties) != VK_SUCCESS)
		return SwapchainError::Error;

	// Happens on nVidia Windows when you minimize a window.
	if (surface_properties.maxImageExtent.width == 0 &&
	    surface_properties.maxImageExtent.height == 0)
		return SwapchainError::NoSurface;

	uint32_t format_count;
	vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &format_count, nullptr);
	vector<VkSurfaceFormatKHR> formats(format_count);
	vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &format_count, formats.data());

	VkSurfaceFormatKHR format;
	if (format_count == 1 && formats[0].format == VK_FORMAT_UNDEFINED)
	{
		format = formats[0];
		format.format = VK_FORMAT_B8G8R8A8_UNORM;
	}
	else
	{
		if (format_count == 0)
		{
			LOGE("Surface has no formats.\n");
			return SwapchainError::Error;
		}

		bool found = false;
		for (unsigned i = 0; i < format_count; i++)
		{
			if (formats[i].format == VK_FORMAT_R8G8B8A8_SRGB ||
			    formats[i].format == VK_FORMAT_B8G8R8A8_SRGB ||
			    formats[i].format == VK_FORMAT_A8B8G8R8_SRGB_PACK32)
			{
				format = formats[i];
				found = true;
			}
		}

		if (!found)
			format = formats[0];
	}

	VkExtent2D swapchain_size;
	if (surface_properties.currentExtent.width == ~0u)
	{
		swapchain_size.width = width;
		swapchain_size.height = height;
	}
	else
	{
		swapchain_size.width = max(min(width, surface_properties.maxImageExtent.width), surface_properties.minImageExtent.width);
		swapchain_size.height = max(min(height, surface_properties.maxImageExtent.height), surface_properties.minImageExtent.height);
	}

	uint32_t num_present_modes;
	vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &num_present_modes, nullptr);
	vector<VkPresentModeKHR> present_modes(num_present_modes);
	vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &num_present_modes, present_modes.data());

	VkPresentModeKHR swapchain_present_mode = VK_PRESENT_MODE_FIFO_KHR;

	const char *vsync = getenv("GRANITE_VULKAN_VSYNC");
	bool use_vsync = !vsync || (strtoul(vsync, nullptr, 0) != 0);
	if (!use_vsync)
	{
		for (uint32_t i = 0; i < num_present_modes; i++)
		{
			if (present_modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR || present_modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
			{
				swapchain_present_mode = present_modes[i];
				break;
			}
		}
	}

	uint32_t desired_swapchain_images = surface_properties.minImageCount + 1;
	if ((surface_properties.maxImageCount > 0) && (desired_swapchain_images > surface_properties.maxImageCount))
		desired_swapchain_images = surface_properties.maxImageCount;

	VkSurfaceTransformFlagBitsKHR pre_transform;
	if (surface_properties.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
		pre_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	else
		pre_transform = surface_properties.currentTransform;

	VkCompositeAlphaFlagBitsKHR composite_mode = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	if (surface_properties.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
		composite_mode = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
	if (surface_properties.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
		composite_mode = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	if (surface_properties.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR)
		composite_mode = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
	if (surface_properties.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)
		composite_mode = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;

	VkSwapchainKHR old_swapchain = swapchain;

	VkSwapchainCreateInfoKHR info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	info.surface = surface;
	info.minImageCount = desired_swapchain_images;
	info.imageFormat = format.format;
	info.imageColorSpace = format.colorSpace;
	info.imageExtent.width = swapchain_size.width;
	info.imageExtent.height = swapchain_size.height;
	info.imageArrayLayers = 1;
	info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.preTransform = pre_transform;
	info.compositeAlpha = composite_mode;
	info.presentMode = swapchain_present_mode;
	info.clipped = VK_TRUE;
	info.oldSwapchain = old_swapchain;

	auto res = vkCreateSwapchainKHR(context->get_device(), &info, nullptr, &swapchain);
	if (old_swapchain != VK_NULL_HANDLE)
		vkDestroySwapchainKHR(context->get_device(), old_swapchain, nullptr);

	if (res != VK_SUCCESS)
	{
		LOGE("Failed to create swapchain (code: %d)\n", int(res));
		swapchain = VK_NULL_HANDLE;
		return SwapchainError::Error;
	}

	this->width = swapchain_size.width;
	this->height = swapchain_size.height;
	this->format = format.format;

	LOGI("Created swapchain %u x %u (fmt: %u).\n", this->width, this->height, static_cast<unsigned>(this->format));

	uint32_t image_count;
	V(vkGetSwapchainImagesKHR(context->get_device(), swapchain, &image_count, nullptr));
	swapchain_images.resize(image_count);
	V(vkGetSwapchainImagesKHR(context->get_device(), swapchain, &image_count, swapchain_images.data()));

	LOGI("Got %u swapchain images.\n", image_count);

	platform->event_swapchain_destroyed();
	platform->event_swapchain_created(device.get(), this->width, this->height, aspect_ratio, image_count, info.imageFormat);

	return SwapchainError::None;
}

WSI::~WSI()
{
	deinit_external();
}
}
