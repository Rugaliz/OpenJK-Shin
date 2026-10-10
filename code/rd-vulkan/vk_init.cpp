/*
===========================================================================
Copyright (C) 2013 - 2018, OpenJK contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

// Vulkan start-up and shutdown for the Vulkan renderer module (docs/vulkan-renderer.md): instance, device,
// swapchain and the render target the whole frame is drawn into.

#include "../server/exe_headers.h"
#include "../rd-vanilla/tr_local.h"
#include "vk_priv.h"

vk_t vk;

static cvar_t	*r_vkValidation;
static cvar_t	*r_vkDevice;

void APIENTRY vkglActiveTextureARB( GLenum texture );
void APIENTRY vkglClientActiveTextureARB( GLenum texture );
void APIENTRY vkglMultiTexCoord2fARB( GLenum target, GLfloat s, GLfloat t );
void APIENTRY vkglMinSampleShadingARB( GLclampf value );
void APIENTRY vkglLockArraysEXT( GLint first, GLsizei count );
void APIENTRY vkglUnlockArraysEXT( void );
void APIENTRY vkglStencilOpSeparate( GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass );

extern bool g_bDynamicGlowSupported;

/*
=================
VK_CheckResult
=================
*/
void VK_CheckResult( VkResult result, const char *what )
{
	if ( result != VK_SUCCESS )
	{
		ri.Error( ERR_FATAL, "Vulkan: %s failed (VkResult %d)", what, (int)result );
	}
}

/*
=================
VK_FindMemoryType
=================
*/
uint32_t VK_FindMemoryType( uint32_t typeBits, VkMemoryPropertyFlags properties )
{
	for ( uint32_t i = 0; i < vk.memoryProperties.memoryTypeCount; i++ )
	{
		if ( ( typeBits & ( 1u << i ) ) && ( vk.memoryProperties.memoryTypes[i].propertyFlags & properties ) == properties )
		{
			return i;
		}
	}
	ri.Error( ERR_FATAL, "Vulkan: no suitable memory type" );
	return 0;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL VK_DebugCallback( VkDebugUtilsMessageSeverityFlagBitsEXT severity,
	VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT *data, void * )
{
	Com_Printf( "%sVulkan: %s\n", ( severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ) ? S_COLOR_RED : S_COLOR_YELLOW, data->pMessage );
	return VK_FALSE;
}

static bool VK_HasInstanceLayer( const char *name )
{
	uint32_t count = 0;
	vkEnumerateInstanceLayerProperties( &count, NULL );
	VkLayerProperties *layers = (VkLayerProperties *)R_Malloc( ( count ? count : 1 ) * sizeof( VkLayerProperties ), TAG_TEMP_WORKSPACE, qfalse );
	vkEnumerateInstanceLayerProperties( &count, layers );
	bool found = false;
	for ( uint32_t i = 0; i < count; i++ )
	{
		if ( !strcmp( layers[i].layerName, name ) )
		{
			found = true;
		}
	}
	R_Free( layers );
	return found;
}

static bool VK_HasInstanceExtension( const char *name )
{
	uint32_t count = 0;
	vkEnumerateInstanceExtensionProperties( NULL, &count, NULL );
	VkExtensionProperties *extensions = (VkExtensionProperties *)R_Malloc( ( count ? count : 1 ) * sizeof( VkExtensionProperties ), TAG_TEMP_WORKSPACE, qfalse );
	vkEnumerateInstanceExtensionProperties( NULL, &count, extensions );
	bool found = false;
	for ( uint32_t i = 0; i < count; i++ )
	{
		if ( !strcmp( extensions[i].extensionName, name ) )
		{
			found = true;
		}
	}
	R_Free( extensions );
	return found;
}

static void VK_CreateInstance( void )
{
	unsigned int sdlExtensionCount = 0;
	const char *const *sdlExtensions = ri.VK_GetInstanceExtensions( &sdlExtensionCount );
	if ( !sdlExtensions )
	{
		ri.Error( ERR_FATAL, "Vulkan: the window system offers no Vulkan surface extensions" );
	}

	const char *extensions[16];
	const char *layers[2];
	uint32_t extensionCount = 0, layerCount = 0;
	for ( unsigned int i = 0; i < sdlExtensionCount && extensionCount < 14; i++ )
	{
		extensions[extensionCount++] = sdlExtensions[i];
	}

	const bool validation = r_vkValidation->integer && VK_HasInstanceLayer( "VK_LAYER_KHRONOS_validation" )
		&& VK_HasInstanceExtension( VK_EXT_DEBUG_UTILS_EXTENSION_NAME );
	if ( r_vkValidation->integer && !validation )
	{
		Com_Printf( "...validation layer requested but not installed (VK_LAYER_KHRONOS_validation)\n" );
	}
	if ( validation )
	{
		layers[layerCount++] = "VK_LAYER_KHRONOS_validation";
		extensions[extensionCount++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
	}

	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app.pApplicationName = "OpenJK";
	app.applicationVersion = 1;
	app.pEngineName = "OpenJK";
	app.engineVersion = 1;
	app.apiVersion = VK_API_VERSION_1_2;	// (1.2 for resolving the depth buffer of a multisampled frame; older devices work without)

	VkInstanceCreateInfo info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	info.pApplicationInfo = &app;
	info.enabledLayerCount = layerCount;
	info.ppEnabledLayerNames = layers;
	info.enabledExtensionCount = extensionCount;
	info.ppEnabledExtensionNames = extensions;
	VK_CheckResult( vkCreateInstance( &info, NULL, &vk.instance ), "vkCreateInstance" );

	if ( validation )
	{
		PFN_vkCreateDebugUtilsMessengerEXT create =
			(PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr( vk.instance, "vkCreateDebugUtilsMessengerEXT" );
		if ( create )
		{
			VkDebugUtilsMessengerCreateInfoEXT messenger = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
			messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
			messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
				| VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
			messenger.pfnUserCallback = VK_DebugCallback;
			create( vk.instance, &messenger, NULL, &vk.messenger );
			Com_Printf( "...validation layer on\n" );
		}
	}

	if ( !ri.VK_CreateSurface( vk.instance, &vk.surface ) )
	{
		ri.Error( ERR_FATAL, "Vulkan: could not create the window surface" );
	}
}

static bool VK_DeviceHasExtension( VkPhysicalDevice device, const char *name )
{
	uint32_t count = 0;
	vkEnumerateDeviceExtensionProperties( device, NULL, &count, NULL );
	VkExtensionProperties *extensions = (VkExtensionProperties *)R_Malloc( ( count ? count : 1 ) * sizeof( VkExtensionProperties ), TAG_TEMP_WORKSPACE, qfalse );
	vkEnumerateDeviceExtensionProperties( device, NULL, &count, extensions );
	bool found = false;
	for ( uint32_t i = 0; i < count; i++ )
	{
		if ( !strcmp( extensions[i].extensionName, name ) )
		{
			found = true;
		}
	}
	R_Free( extensions );
	return found;
}

// graphics + present queue family of a device, or -1
static int VK_FindQueueFamily( VkPhysicalDevice device )
{
	uint32_t count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties( device, &count, NULL );
	VkQueueFamilyProperties *families = (VkQueueFamilyProperties *)R_Malloc( ( count ? count : 1 ) * sizeof( VkQueueFamilyProperties ), TAG_TEMP_WORKSPACE, qfalse );
	vkGetPhysicalDeviceQueueFamilyProperties( device, &count, families );
	int found = -1;
	for ( uint32_t i = 0; i < count && found < 0; i++ )
	{
		VkBool32 present = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR( device, i, vk.surface, &present );
		if ( present && ( families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT ) )
		{
			found = (int)i;
		}
	}
	R_Free( families );
	return found;
}

static void VK_CreateDevice( void )
{
	uint32_t count = 0;
	vkEnumeratePhysicalDevices( vk.instance, &count, NULL );
	if ( !count )
	{
		ri.Error( ERR_FATAL, "Vulkan: no Vulkan device found" );
	}
	VkPhysicalDevice *devices = (VkPhysicalDevice *)R_Malloc( count * sizeof( VkPhysicalDevice ), TAG_TEMP_WORKSPACE, qfalse );
	vkEnumeratePhysicalDevices( vk.instance, &count, devices );

	// r_vkDevice -1 picks the best one (a discrete GPU), otherwise it is an index in the list printed at start-up
	int best = -1, bestScore = -1;
	for ( uint32_t i = 0; i < count; i++ )
	{
		VkPhysicalDeviceProperties properties;
		vkGetPhysicalDeviceProperties( devices[i], &properties );
		const int family = VK_FindQueueFamily( devices[i] );
		const bool usable = family >= 0 && VK_DeviceHasExtension( devices[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME );
		Com_Printf( "...device %u: %s%s\n", i, properties.deviceName, usable ? "" : " (cannot present to this window)" );
		if ( !usable )
		{
			continue;
		}
		int score = 1;
		if ( properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ) score = 1000;
		else if ( properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ) score = 500;
		if ( r_vkDevice->integer == (int)i ) score = 100000;
		if ( score > bestScore )
		{
			bestScore = score;
			best = (int)i;
		}
	}
	if ( best < 0 )
	{
		ri.Error( ERR_FATAL, "Vulkan: no device can present to the window" );
	}

	vk.physicalDevice = devices[best];
	R_Free( devices );
	vk.queueFamily = (uint32_t)VK_FindQueueFamily( vk.physicalDevice );
	vkGetPhysicalDeviceProperties( vk.physicalDevice, &vk.properties );
	vkGetPhysicalDeviceMemoryProperties( vk.physicalDevice, &vk.memoryProperties );
	vkGetPhysicalDeviceFeatures( vk.physicalDevice, &vk.features );

	VkPhysicalDeviceFeatures enabled = {};
	enabled.samplerAnisotropy = vk.features.samplerAnisotropy;
	enabled.fillModeNonSolid = vk.features.fillModeNonSolid;
	enabled.wideLines = vk.features.wideLines;
	enabled.sampleRateShading = vk.features.sampleRateShading;
	vk.features = enabled;	// from here on: what is on

	const float priority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	queueInfo.queueFamilyIndex = vk.queueFamily;
	queueInfo.queueCount = 1;
	queueInfo.pQueuePriorities = &priority;

	const char *extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	VkDeviceCreateInfo info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	info.queueCreateInfoCount = 1;
	info.pQueueCreateInfos = &queueInfo;
	info.enabledExtensionCount = 1;
	info.ppEnabledExtensionNames = extensions;
	info.pEnabledFeatures = &enabled;
	VK_CheckResult( vkCreateDevice( vk.physicalDevice, &info, NULL, &vk.device ), "vkCreateDevice" );
	vkGetDeviceQueue( vk.device, vk.queueFamily, 0, &vk.queue );
}

static VkFormat VK_FindDepthFormat( void )
{
	const VkFormat candidates[] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT };
	for ( int i = 0; i < 2; i++ )
	{
		VkFormatProperties properties;
		vkGetPhysicalDeviceFormatProperties( vk.physicalDevice, candidates[i], &properties );
		if ( properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT )
		{
			return candidates[i];
		}
	}
	ri.Error( ERR_FATAL, "Vulkan: no depth and stencil format" );
	return VK_FORMAT_UNDEFINED;
}

static void VK_CreateImage( VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkSampleCountFlagBits samples,
	VkImage *image, VkDeviceMemory *memory, VkImageView *view )
{
	VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = format;
	info.extent.width = (uint32_t)vk.width;
	info.extent.height = (uint32_t)vk.height;
	info.extent.depth = 1;
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = samples;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VK_CheckResult( vkCreateImage( vk.device, &info, NULL, image ), "vkCreateImage" );

	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements( vk.device, *image, &requirements );
	VkMemoryAllocateInfo allocate = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = VK_FindMemoryType( requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
	VK_CheckResult( vkAllocateMemory( vk.device, &allocate, NULL, memory ), "vkAllocateMemory" );
	VK_CheckResult( vkBindImageMemory( vk.device, *image, *memory, 0 ), "vkBindImageMemory" );

	VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	viewInfo.image = *image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = format;
	viewInfo.subresourceRange.aspectMask = aspect;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = 1;
	VK_CheckResult( vkCreateImageView( vk.device, &viewInfo, NULL, view ), "vkCreateImageView" );
}

static VkRenderPass VK_CreateRenderPass( bool clear )
{
	VkAttachmentDescription attachments[2] = {};
	attachments[0].format = vk.colorFormat;
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	// (the colour is never cleared by the pass: like the back buffer of OpenGL, what was drawn last frame stays until it
	// is drawn over, which is what a screenshot taken at the start of a frame expects)
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	attachments[1].format = vk.depthFormat;
	attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[1].loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
	attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[1].stencilLoadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkAttachmentReference colorRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference depthRef = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	VkSubpassDescription subpass = {};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorRef;
	subpass.pDepthStencilAttachment = &depthRef;

	// the pass is the only user of the images between barriers the renderer adds itself, so one external
	// dependency each way is enough
	VkSubpassDependency dependencies[2] = {};
	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass = 0;
	dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
		| VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
		| VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
		| VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
	dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
		| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dependencies[1].srcSubpass = 0;
	dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[1].srcStageMask = dependencies[0].dstStageMask;
	dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
		| VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[1].srcAccessMask = dependencies[0].dstAccessMask;
	dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT
		| VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	VkRenderPassCreateInfo info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	info.attachmentCount = 2;
	info.pAttachments = attachments;
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	info.dependencyCount = 2;
	info.pDependencies = dependencies;
	VkRenderPass pass;
	VK_CheckResult( vkCreateRenderPass( vk.device, &info, NULL, &pass ), "vkCreateRenderPass" );
	return pass;
}

// the pass of a multisampled frame: colour and depth are drawn at vk.samples and resolved into the single sample images,
// which everything else (read backs, copies, the final blit) works with
static VkRenderPass VK_CreateRenderPassMultisampled( bool clear )
{
	VkAttachmentDescription2 attachments[4] = {};
	for ( int i = 0; i < 4; i++ )
	{
		attachments[i].sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
	}
	attachments[0].format = vk.colorFormat;
	attachments[0].samples = vk.samples;
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;		// (as in the single sample pass, the colour is not cleared)
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	attachments[1].format = vk.depthFormat;
	attachments[1].samples = vk.samples;
	attachments[1].loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
	attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[1].stencilLoadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[1].initialLayout = attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	attachments[2].format = vk.colorFormat;
	attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[2].initialLayout = attachments[2].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	attachments[3].format = vk.depthFormat;
	attachments[3].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[3].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[3].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[3].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[3].initialLayout = attachments[3].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkAttachmentReference2 colorRef = { VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2 };
	colorRef.attachment = 0;
	colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	colorRef.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	VkAttachmentReference2 depthRef = { VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2 };
	depthRef.attachment = 1;
	depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	depthRef.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	VkAttachmentReference2 resolveColorRef = { VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2 };
	resolveColorRef.attachment = 2;
	resolveColorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	resolveColorRef.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	VkAttachmentReference2 resolveDepthRef = { VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2 };
	resolveDepthRef.attachment = 3;
	resolveDepthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	resolveDepthRef.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

	VkSubpassDescriptionDepthStencilResolve depthResolve = { VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE };
	depthResolve.depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
	depthResolve.stencilResolveMode = vk.stencilResolveMode;
	depthResolve.pDepthStencilResolveAttachment = &resolveDepthRef;

	VkSubpassDescription2 subpass = { VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2 };
	subpass.pNext = &depthResolve;
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorRef;
	subpass.pResolveAttachments = &resolveColorRef;
	subpass.pDepthStencilAttachment = &depthRef;

	VkSubpassDependency2 dependencies[2] = { { VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2 }, { VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2 } };
	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass = 0;
	dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
		| VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
		| VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
		| VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
	dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
		| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dependencies[1].srcSubpass = 0;
	dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[1].srcStageMask = dependencies[0].dstStageMask;
	dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
		| VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[1].srcAccessMask = dependencies[0].dstAccessMask;
	dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT
		| VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	VkRenderPassCreateInfo2 info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2 };
	info.attachmentCount = 4;
	info.pAttachments = attachments;
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	info.dependencyCount = 2;
	info.pDependencies = dependencies;
	VkRenderPass pass;
	VK_CheckResult( vkCreateRenderPass2( vk.device, &info, NULL, &pass ), "vkCreateRenderPass2" );
	return pass;
}

// how many samples the frame can be drawn at: what is wanted (r_ext_multisample) as far as the device allows
static VkSampleCountFlagBits VK_ChooseSamples( int wanted )
{
	vk.stencilResolveMode = VK_RESOLVE_MODE_NONE;
	if ( wanted < 2 )
	{
		return VK_SAMPLE_COUNT_1_BIT;
	}
	if ( vk.properties.apiVersion < VK_API_VERSION_1_2 )
	{
		Com_Printf( "...multisampling needs Vulkan 1.2 (the device has less)\n" );
		return VK_SAMPLE_COUNT_1_BIT;
	}

	// the depth buffer has to be resolved as well (the flares read it back): one sample of it is taken
	VkPhysicalDeviceDepthStencilResolveProperties resolve = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES };
	VkPhysicalDeviceProperties2 properties = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
	properties.pNext = &resolve;
	vkGetPhysicalDeviceProperties2( vk.physicalDevice, &properties );
	if ( !( resolve.supportedDepthResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT ) )
	{
		Com_Printf( "...multisampling: the device cannot resolve the depth buffer\n" );
		return VK_SAMPLE_COUNT_1_BIT;
	}
	if ( resolve.independentResolveNone )
	{
		vk.stencilResolveMode = VK_RESOLVE_MODE_NONE;
	}
	else if ( resolve.supportedStencilResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT )
	{
		vk.stencilResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
	}
	else
	{
		Com_Printf( "...multisampling: the device cannot resolve the stencil buffer\n" );
		return VK_SAMPLE_COUNT_1_BIT;
	}

	const VkSampleCountFlags supported = vk.properties.limits.framebufferColorSampleCounts
		& vk.properties.limits.framebufferDepthSampleCounts & vk.properties.limits.framebufferStencilSampleCounts;
	const VkSampleCountFlagBits steps[] = { VK_SAMPLE_COUNT_16_BIT, VK_SAMPLE_COUNT_8_BIT, VK_SAMPLE_COUNT_4_BIT, VK_SAMPLE_COUNT_2_BIT };
	const int counts[] = { 16, 8, 4, 2 };
	for ( int i = 0; i < 4; i++ )
	{
		if ( counts[i] <= wanted && ( supported & steps[i] ) )
		{
			return steps[i];
		}
	}
	return VK_SAMPLE_COUNT_1_BIT;
}

static void VK_CreateRenderTarget( void )
{
	vk.colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
	vk.depthFormat = VK_FindDepthFormat();

	VK_CreateImage( vk.colorFormat,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT, &vk.colorImage, &vk.colorMemory, &vk.colorView );
	VK_CreateImage( vk.depthFormat,
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, VK_SAMPLE_COUNT_1_BIT, &vk.depthImage, &vk.depthMemory, &vk.depthView );
	if ( vk.samples != VK_SAMPLE_COUNT_1_BIT )
	{
		VK_CreateImage( vk.colorFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			VK_IMAGE_ASPECT_COLOR_BIT, vk.samples, &vk.msaaColorImage, &vk.msaaColorMemory, &vk.msaaColorView );
		VK_CreateImage( vk.depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
			VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, vk.samples, &vk.msaaDepthImage, &vk.msaaDepthMemory, &vk.msaaDepthView );
	}

	const bool multisampled = vk.samples != VK_SAMPLE_COUNT_1_BIT;
	vk.renderPassClear = multisampled ? VK_CreateRenderPassMultisampled( true ) : VK_CreateRenderPass( true );
	vk.renderPassLoad = multisampled ? VK_CreateRenderPassMultisampled( false ) : VK_CreateRenderPass( false );

	// (multisampled: the multisampled images first, the single sample ones they are resolved into after)
	const VkImageView views[4] = { multisampled ? vk.msaaColorView : vk.colorView, multisampled ? vk.msaaDepthView : vk.depthView,
		vk.colorView, vk.depthView };
	VkFramebufferCreateInfo info = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
	info.renderPass = vk.renderPassClear;
	info.attachmentCount = multisampled ? 4 : 2;
	info.pAttachments = views;
	info.width = (uint32_t)vk.width;
	info.height = (uint32_t)vk.height;
	info.layers = 1;
	VK_CheckResult( vkCreateFramebuffer( vk.device, &info, NULL, &vk.framebuffer ), "vkCreateFramebuffer" );

	// both images start in the layout the render passes expect
	VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	poolInfo.queueFamilyIndex = vk.queueFamily;
	VkCommandPool pool;
	VK_CheckResult( vkCreateCommandPool( vk.device, &poolInfo, NULL, &pool ), "vkCreateCommandPool" );
	VkCommandBufferAllocateInfo allocate = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	allocate.commandPool = pool;
	allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate.commandBufferCount = 1;
	VkCommandBuffer cmd;
	VK_CheckResult( vkAllocateCommandBuffers( vk.device, &allocate, &cmd ), "vkAllocateCommandBuffers" );
	VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer( cmd, &begin );
	// the colour starts out black
	VK_ImageBarrier( cmd, vk.colorImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT );
	const VkClearColorValue black = {};
	const VkImageSubresourceRange colorRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	vkCmdClearColorImage( cmd, vk.colorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &colorRange );
	VK_ImageBarrier( cmd, vk.colorImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT );
	VK_ImageBarrier( cmd, vk.depthImage, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT );
	if ( multisampled )
	{
		VK_ImageBarrier( cmd, vk.msaaColorImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT );
		vkCmdClearColorImage( cmd, vk.msaaColorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &colorRange );
		VK_ImageBarrier( cmd, vk.msaaColorImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT );
		VK_ImageBarrier( cmd, vk.msaaDepthImage, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT );
	}
	vkEndCommandBuffer( cmd );
	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	vkQueueSubmit( vk.queue, 1, &submit, VK_NULL_HANDLE );
	vkQueueWaitIdle( vk.queue );
	vkDestroyCommandPool( vk.device, pool, NULL );
}

static void VK_DestroyRenderTarget( void )
{
	vkDestroyFramebuffer( vk.device, vk.framebuffer, NULL );
	vkDestroyRenderPass( vk.device, vk.renderPassClear, NULL );
	vkDestroyRenderPass( vk.device, vk.renderPassLoad, NULL );
	if ( vk.samples != VK_SAMPLE_COUNT_1_BIT )
	{
		vkDestroyImageView( vk.device, vk.msaaDepthView, NULL );
		vkDestroyImage( vk.device, vk.msaaDepthImage, NULL );
		vkFreeMemory( vk.device, vk.msaaDepthMemory, NULL );
		vkDestroyImageView( vk.device, vk.msaaColorView, NULL );
		vkDestroyImage( vk.device, vk.msaaColorImage, NULL );
		vkFreeMemory( vk.device, vk.msaaColorMemory, NULL );
	}
	vkDestroyImageView( vk.device, vk.depthView, NULL );
	vkDestroyImage( vk.device, vk.depthImage, NULL );
	vkFreeMemory( vk.device, vk.depthMemory, NULL );
	vkDestroyImageView( vk.device, vk.colorView, NULL );
	vkDestroyImage( vk.device, vk.colorImage, NULL );
	vkFreeMemory( vk.device, vk.colorMemory, NULL );
}

static VkPresentModeKHR VK_ChoosePresentMode( void )
{
	uint32_t count = 0;
	vkGetPhysicalDeviceSurfacePresentModesKHR( vk.physicalDevice, vk.surface, &count, NULL );
	VkPresentModeKHR modes[16];
	if ( count > 16 ) count = 16;
	vkGetPhysicalDeviceSurfacePresentModesKHR( vk.physicalDevice, vk.surface, &count, modes );
	bool immediate = false, relaxed = false, mailbox = false;
	for ( uint32_t i = 0; i < count; i++ )
	{
		if ( modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR ) immediate = true;
		if ( modes[i] == VK_PRESENT_MODE_FIFO_RELAXED_KHR ) relaxed = true;
		if ( modes[i] == VK_PRESENT_MODE_MAILBOX_KHR ) mailbox = true;
	}

	// r_swapInterval 0: no waiting; 1: vsync (with r_adaptiveVsync a late frame tears instead of waiting a whole refresh)
	cvar_t *swapInterval = ri.Cvar_Get( "r_swapInterval", "0", CVAR_ARCHIVE_ND );
	cvar_t *adaptive = ri.Cvar_Get( "r_adaptiveVsync", "0", CVAR_ARCHIVE_ND );
	if ( swapInterval->integer == 0 )
	{
		return immediate ? VK_PRESENT_MODE_IMMEDIATE_KHR : ( mailbox ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR );
	}
	if ( adaptive->integer && relaxed )
	{
		return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
	}
	return VK_PRESENT_MODE_FIFO_KHR;
}

void VK_CreateSwapchain( void )
{
	VkSurfaceCapabilitiesKHR capabilities;
	VK_CheckResult( vkGetPhysicalDeviceSurfaceCapabilitiesKHR( vk.physicalDevice, vk.surface, &capabilities ), "surface capabilities" );

	uint32_t formatCount = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR( vk.physicalDevice, vk.surface, &formatCount, NULL );
	VkSurfaceFormatKHR formats[32];
	if ( formatCount > 32 ) formatCount = 32;
	vkGetPhysicalDeviceSurfaceFormatsKHR( vk.physicalDevice, vk.surface, &formatCount, formats );
	VkSurfaceFormatKHR chosen = formats[0];
	for ( uint32_t i = 0; i < formatCount; i++ )
	{
		// plain (not sRGB) 8 bit, the game does its own gamma
		if ( formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM )
		{
			chosen = formats[i];
			break;
		}
	}

	int drawableWidth = 0, drawableHeight = 0;
	ri.VK_GetDrawableSize( &drawableWidth, &drawableHeight );
	VkExtent2D extent = capabilities.currentExtent;
	if ( extent.width == 0xFFFFFFFFu )
	{
		extent.width = (uint32_t)drawableWidth;
		extent.height = (uint32_t)drawableHeight;
	}
	if ( extent.width < capabilities.minImageExtent.width ) extent.width = capabilities.minImageExtent.width;
	if ( extent.height < capabilities.minImageExtent.height ) extent.height = capabilities.minImageExtent.height;
	if ( extent.width > capabilities.maxImageExtent.width ) extent.width = capabilities.maxImageExtent.width;
	if ( extent.height > capabilities.maxImageExtent.height ) extent.height = capabilities.maxImageExtent.height;

	uint32_t imageCount = capabilities.minImageCount + 1;
	if ( capabilities.maxImageCount && imageCount > capabilities.maxImageCount ) imageCount = capabilities.maxImageCount;
	if ( imageCount > VK_MAX_SWAPCHAIN_IMAGES ) imageCount = VK_MAX_SWAPCHAIN_IMAGES;

	vk.presentMode = VK_ChoosePresentMode();

	VkSwapchainCreateInfoKHR info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	info.surface = vk.surface;
	info.minImageCount = imageCount;
	info.imageFormat = chosen.format;
	info.imageColorSpace = chosen.colorSpace;
	info.imageExtent = extent;
	info.imageArrayLayers = 1;
	info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.preTransform = capabilities.currentTransform;
	info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	info.presentMode = vk.presentMode;
	info.clipped = VK_TRUE;
	info.oldSwapchain = vk.swapchain;

	VkSwapchainKHR swapchain;
	VK_CheckResult( vkCreateSwapchainKHR( vk.device, &info, NULL, &swapchain ), "vkCreateSwapchainKHR" );
	if ( vk.swapchain )
	{
		VK_DestroySwapchain();
	}
	vk.swapchain = swapchain;
	vk.swapchainFormat = chosen.format;
	vk.swapchainExtent = extent;

	vk.swapchainImageCount = VK_MAX_SWAPCHAIN_IMAGES;
	VK_CheckResult( vkGetSwapchainImagesKHR( vk.device, vk.swapchain, &vk.swapchainImageCount, vk.swapchainImages ), "swapchain images" );

	VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	for ( uint32_t i = 0; i < vk.swapchainImageCount; i++ )
	{
		VK_CheckResult( vkCreateSemaphore( vk.device, &semaphoreInfo, NULL, &vk.renderFinished[i] ), "vkCreateSemaphore" );
	}
	vk.swapchainDirty = false;
}

// destroys the semaphores of the images and the swapchain itself (the new one, if there is one, is made already)
void VK_DestroySwapchain( void )
{
	vkDeviceWaitIdle( vk.device );
	for ( uint32_t i = 0; i < vk.swapchainImageCount; i++ )
	{
		vkDestroySemaphore( vk.device, vk.renderFinished[i], NULL );
		vk.renderFinished[i] = VK_NULL_HANDLE;
	}
	vk.swapchainImageCount = 0;
}

static void VK_CreateFrames( void )
{
	for ( int i = 0; i < VK_FRAMES_IN_FLIGHT; i++ )
	{
		VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		poolInfo.queueFamilyIndex = vk.queueFamily;
		VK_CheckResult( vkCreateCommandPool( vk.device, &poolInfo, NULL, &vk.frames[i].pool ), "vkCreateCommandPool" );
		VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		VK_CheckResult( vkCreateFence( vk.device, &fenceInfo, NULL, &vk.frames[i].fence ), "vkCreateFence" );
		VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		VK_CheckResult( vkCreateSemaphore( vk.device, &semaphoreInfo, NULL, &vk.frames[i].imageAvailable ), "vkCreateSemaphore" );
		vk.frames[i].fenceSubmitted = false;
		vk.frameCommandBufferCount[i] = 0;
	}
}

/*
=================
VK_Init

Called by InitOpenGL once the window exists.
=================
*/
void VK_Init( glconfig_t *glConfig )
{
	Com_Printf( "Initializing Vulkan\n" );

	r_vkValidation = ri.Cvar_Get( "r_vkValidation", "0", CVAR_ARCHIVE_ND | CVAR_LATCH );
	r_vkDevice = ri.Cvar_Get( "r_vkDevice", "-1", CVAR_ARCHIVE_ND | CVAR_LATCH );
	r_vkProfile = ri.Cvar_Get( "r_vkProfile", "0", 0 );

	memset( &vk, 0, sizeof( vk ) );
	vk.width = glConfig->vidWidth;
	vk.height = glConfig->vidHeight;
	vk.clearDepth = 1.0f;

	VK_CreateInstance();
	VK_CreateDevice();
	{
		cvar_t *multisample = ri.Cvar_Get( "r_ext_multisample", "0", CVAR_ARCHIVE_ND | CVAR_LATCH );
		vk.samples = VK_ChooseSamples( multisample->integer );
		const int actual = vk.samples == VK_SAMPLE_COUNT_1_BIT ? 0 : (int)vk.samples;
		if ( actual != multisample->integer )
		{
			ri.Cvar_Set( "r_ext_multisample", va( "%d", actual ) );	// (so that the menu shows what is in use)
		}
	}
	VK_CreateRenderTarget();
	VK_CreateSwapchain();
	VK_CreateFrames();
	VK_InitImages();
	VK_InitDraw();
	VK_InitPost();
	VK_ResetGLState();

	Com_Printf( "...using %s\n", vk.properties.deviceName );

	static char renderer[256], version[64], extensions[64];
	Q_strncpyz( renderer, vk.properties.deviceName, sizeof( renderer ) );
	Com_sprintf( version, sizeof( version ), "Vulkan %u.%u.%u", VK_API_VERSION_MAJOR( vk.properties.apiVersion ),
		VK_API_VERSION_MINOR( vk.properties.apiVersion ), VK_API_VERSION_PATCH( vk.properties.apiVersion ) );
	Q_strncpyz( extensions, "", sizeof( extensions ) );
	glConfig->vendor_string = "Vulkan";
	glConfig->renderer_string = renderer;
	glConfig->version_string = version;
	glConfig->extensions_string = extensions;

	glConfig->colorBits = 24;
	glConfig->depthBits = 24;
	glConfig->stencilBits = 8;

	glConfig->maxTextureSize = Q_min( (int)vk.properties.limits.maxImageDimension2D, 8192 );
	glConfig->maxActiveTextures = 4;
	glConfig->clampToEdgeAvailable = qtrue;
	glConfig->textureEnvAddAvailable = qtrue;
	glConfig->textureCompression = TC_NONE;
	glConfig->doStencilShadowsInOneDrawcall = qtrue;
	glConfig->maxTextureFilterAnisotropy = vk.features.samplerAnisotropy ? vk.properties.limits.maxSamplerAnisotropy : 0.0f;
	ri.Cvar_SetValue( "r_ext_texture_filter_anisotropic_avail", glConfig->maxTextureFilterAnisotropy );

	// what the layer implements (everything else stays NULL: no GLSL, ARB programs, register combiners, framebuffers)
	qglActiveTextureARB = vkglActiveTextureARB;
	qglClientActiveTextureARB = vkglClientActiveTextureARB;
	qglMultiTexCoord2fARB = vkglMultiTexCoord2fARB;
	qglLockArraysEXT = vkglLockArraysEXT;
	qglUnlockArraysEXT = vkglUnlockArraysEXT;
	qglStencilOpSeparate = vkglStencilOpSeparate;
	qglMinSampleShadingARB = ( vk.samples != VK_SAMPLE_COUNT_1_BIT && vk.features.sampleRateShading ) ? vkglMinSampleShadingARB : NULL;

	g_bDynamicGlowSupported = false;
	ri.Cvar_Set( "r_DynamicGlow", "0" );

	R_GLSL_Init();
	R_Post_Init();

	// the renderer ends a frame with ri.WIN_Present: the Vulkan module presents itself, the window does not
	ri.WIN_Present = VK_PresentFrame;
}

void VK_Shutdown( void )
{
	if ( !vk.device )
	{
		return;
	}
	vkDeviceWaitIdle( vk.device );
	VK_ShutdownReadback();
	VK_ShutdownPost();
	VK_ShutdownDraw();
	VK_ShutdownImages();

	for ( int i = 0; i < VK_FRAMES_IN_FLIGHT; i++ )
	{
		vkDestroySemaphore( vk.device, vk.frames[i].imageAvailable, NULL );
		vkDestroyFence( vk.device, vk.frames[i].fence, NULL );
		vkDestroyCommandPool( vk.device, vk.frames[i].pool, NULL );
	}
	VK_DestroySwapchain();
	vkDestroySwapchainKHR( vk.device, vk.swapchain, NULL );
	VK_DestroyRenderTarget();
	vkDestroyDevice( vk.device, NULL );
	vkDestroySurfaceKHR( vk.instance, vk.surface, NULL );
	if ( vk.messenger )
	{
		PFN_vkDestroyDebugUtilsMessengerEXT destroy =
			(PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr( vk.instance, "vkDestroyDebugUtilsMessengerEXT" );
		if ( destroy ) destroy( vk.instance, vk.messenger, NULL );
	}
	vkDestroyInstance( vk.instance, NULL );
	memset( &vk, 0, sizeof( vk ) );
}
