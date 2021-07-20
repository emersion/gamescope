// Initialize Vulkan and composite stuff with a compute queue

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <array>

#include "rendervulkan.hpp"
#include "main.hpp"
#include "steamcompmgr.hpp"
#include "sdlwindow.hpp"

#include "composite.h"

PFN_vkGetMemoryFdKHR dyn_vkGetMemoryFdKHR;
PFN_vkGetImageDrmFormatModifierPropertiesEXT dyn_vkGetImageDrmFormatModifierPropertiesEXT;
PFN_vkGetFenceFdKHR dyn_vkGetFenceFdKHR;

const VkApplicationInfo appInfo = {
	.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
	.pApplicationName = "gamescope",
	.applicationVersion = VK_MAKE_VERSION(1, 0, 0),
	.pEngineName = "just some code",
	.engineVersion = VK_MAKE_VERSION(1, 0, 0),
	.apiVersion = VK_API_VERSION_1_1,
};

std::vector< const char * > g_vecSDLInstanceExts;

VkInstance instance;

#define k_nMaxSets 8 // should only need one or two per output tops

struct VulkanOutput_t
{
	VkSurfaceKHR surface;
	VkSurfaceCapabilitiesKHR surfaceCaps;
	std::vector< VkSurfaceFormatKHR > surfaceFormats;
	std::vector< VkPresentModeKHR > presentModes;
	uint32_t nSwapChainImageIndex;
	
	VkSwapchainKHR swapChain;
	std::vector< VkImage > swapChainImages;
	std::vector< VkImageView > swapChainImageViews;
	
	// If no swapchain, use our own images
	
	int nOutImage; // ping/pong between two RTs
	CVulkanTexture outputImage[2];

	VkFormat outputFormat;

	int nCurCmdBuffer;
	VkCommandBuffer commandBuffers[2]; // ping/pong command buffers as well
	
	VkBuffer constantBuffer;
	VkDeviceMemory bufferMemory;
	Composite_t::CompositeData_t *pCompositeBuffer;

	VkFence fence;
	int fenceFD;

	CVulkanTexture *pScreenshotImage;
};


VkPhysicalDevice physicalDevice;
uint32_t queueFamilyIndex;
VkQueue queue;
VkShaderModule shaderModule;
VkDevice device;
VkCommandPool commandPool;
VkDescriptorPool descriptorPool;

bool g_vulkanSupportsModifiers;

VkDescriptorSetLayout descriptorSetLayout;
VkPipelineLayout pipelineLayout;
VkDescriptorSet descriptorSet;

std::array<std::array<std::array<VkPipeline, k_nMaxYcbcrMask>, 2>, k_nMaxLayers> pipelines;

VkBuffer uploadBuffer;
VkDeviceMemory uploadBufferMemory;
void *pUploadBuffer;

const uint32_t k_nScratchCmdBufferCount = 1000;

struct scratchCmdBuffer_t
{
	VkCommandBuffer cmdBuf;
	VkFence fence;
	
	std::vector<CVulkanTexture *> refs;
	
	std::atomic<bool> haswaiter;
	std::atomic<bool> busy;
};

scratchCmdBuffer_t g_scratchCommandBuffers[ k_nScratchCmdBufferCount ];

struct VkPhysicalDeviceMemoryProperties memoryProperties;

VulkanOutput_t g_output;

std::unordered_map<VulkanTexture_t, CVulkanTexture *> g_mapVulkanTextures;
std::atomic<VulkanTexture_t> g_nMaxVulkanTexHandle;

struct VulkanSamplerCacheEntry_t
{
	VulkanPipeline_t::LayerBinding_t key;
	VkSampler sampler;
};

std::vector< VulkanSamplerCacheEntry_t > g_vecVulkanSamplerCache;

VulkanTexture_t g_emptyTex;

static std::map< VkFormat, std::map< uint64_t, VkDrmFormatModifierPropertiesEXT > > DRMModifierProps = {};
static std::vector< uint32_t > sampledShmFormats{};
static struct wlr_drm_format_set sampledDRMFormats = {};

#define MAX_DEVICE_COUNT 8
#define MAX_QUEUE_COUNT 8

#define VK_STRUCTURE_TYPE_WSI_IMAGE_CREATE_INFO_MESA (VkStructureType)1000001002
#define VK_STRUCTURE_TYPE_WSI_MEMORY_ALLOCATE_INFO_MESA (VkStructureType)1000001003

struct wsi_image_create_info {
	VkStructureType sType;
	const void *pNext;
	bool scanout;
	
	uint32_t modifier_count;
	const uint64_t *modifiers;
};

struct wsi_memory_allocate_info {
	VkStructureType sType;
	const void *pNext;
	bool implicit_sync;
};

struct {
	uint32_t DRMFormat;
	VkFormat vkFormat;
	bool bNeedsSwizzle;
	bool bHasAlpha;
} s_DRMVKFormatTable[] = {
	{ DRM_FORMAT_XRGB8888, VK_FORMAT_A8B8G8R8_UNORM_PACK32, true, false },
	{ DRM_FORMAT_ARGB8888, VK_FORMAT_A8B8G8R8_UNORM_PACK32, true, true },
	{ DRM_FORMAT_ARGB8888, VK_FORMAT_R8G8B8A8_UNORM, false, true },
	{ DRM_FORMAT_NV12, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, false, false },
	{ DRM_FORMAT_INVALID, VK_FORMAT_UNDEFINED, false, false },
};

static inline uint32_t VulkanFormatToDRM( VkFormat vkFormat )
{
	for ( int i = 0; s_DRMVKFormatTable[i].vkFormat != VK_FORMAT_UNDEFINED; i++ )
	{
		if ( s_DRMVKFormatTable[i].vkFormat == vkFormat )
		{
			return s_DRMVKFormatTable[i].DRMFormat;
		}
	}
	
	return DRM_FORMAT_INVALID;
}

static inline VkFormat DRMFormatToVulkan( uint32_t nDRMFormat )
{
	for ( int i = 0; s_DRMVKFormatTable[i].vkFormat != VK_FORMAT_UNDEFINED; i++ )
	{
		if ( s_DRMVKFormatTable[i].DRMFormat == nDRMFormat )
		{
			return s_DRMVKFormatTable[i].vkFormat;
		}
	}
	
	return VK_FORMAT_UNDEFINED;
}

static inline bool DRMFormatNeedsSwizzle( uint32_t nDRMFormat )
{
	for ( int i = 0; s_DRMVKFormatTable[i].vkFormat != VK_FORMAT_UNDEFINED; i++ )
	{
		if ( s_DRMVKFormatTable[i].DRMFormat == nDRMFormat )
		{
			return s_DRMVKFormatTable[i].bNeedsSwizzle;
		}
	}
	
	return false;
}

static inline bool DRMFormatHasAlpha( uint32_t nDRMFormat )
{
	for ( int i = 0; s_DRMVKFormatTable[i].vkFormat != VK_FORMAT_UNDEFINED; i++ )
	{
		if ( s_DRMVKFormatTable[i].DRMFormat == nDRMFormat )
		{
			return s_DRMVKFormatTable[i].bHasAlpha;
		}
	}
	
	return false;
}

int32_t findMemoryType( VkMemoryPropertyFlags properties, uint32_t requiredTypeBits )
{
	for ( uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++ )
	{
		if ( ( ( 1 << i ) & requiredTypeBits ) == 0 )
			continue;
		
		if ( ( properties & memoryProperties.memoryTypes[ i ].propertyFlags ) != properties )
			continue;
		
		return i;
	}
	
	return -1;
}

static bool allDMABUFsEqual( wlr_dmabuf_attributes *pDMA )
{
	if ( pDMA->n_planes == 1 )
		return true;

	struct stat first_stat;
	if ( fstat( pDMA->fd[0], &first_stat ) != 0 )
	{
		perror( "fstat failed" );
		return false;
	}

	for ( int i = 1; i < pDMA->n_planes; ++i )
	{
		struct stat plane_stat;
		if ( fstat( pDMA->fd[i], &plane_stat ) != 0 )
		{
			perror( "fstat failed" );
			return false;
		}
		if ( plane_stat.st_ino != first_stat.st_ino )
			return false;
	}

	return true;
}

static VkResult getModifierProps( const VkImageCreateInfo *imageInfo, uint64_t modifier, VkExternalImageFormatProperties *externalFormatProps)
{
	VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifierFormatInfo = {};
	VkPhysicalDeviceExternalImageFormatInfo externalImageFormatInfo = {};
	VkPhysicalDeviceImageFormatInfo2 imageFormatInfo = {};
	VkImageFormatProperties2 imageProps = {};

	modifierFormatInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
	modifierFormatInfo.drmFormatModifier = modifier;
	modifierFormatInfo.sharingMode = imageInfo->sharingMode;

	externalImageFormatInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
	externalImageFormatInfo.pNext = &modifierFormatInfo;
	externalImageFormatInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

	imageFormatInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
	imageFormatInfo.pNext = &externalImageFormatInfo;
	imageFormatInfo.format = imageInfo->format;
	imageFormatInfo.type = imageInfo->imageType;
	imageFormatInfo.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	imageFormatInfo.usage = imageInfo->usage;
	imageFormatInfo.flags = imageInfo->flags;

	imageProps.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
	imageProps.pNext = externalFormatProps;

	return vkGetPhysicalDeviceImageFormatProperties2(physicalDevice, &imageFormatInfo, &imageProps);
}

bool CVulkanTexture::BInit( uint32_t width, uint32_t height, VkFormat format, createFlags flags, wlr_dmabuf_attributes *pDMA /* = nullptr */ )
{
	VkResult res = VK_ERROR_INITIALIZATION_FAILED;

	VkImageTiling tiling = (flags.bMappable || flags.bLinear) ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
	VkImageUsageFlags usage = flags.bTextureable ? VK_IMAGE_USAGE_SAMPLED_BIT : VK_IMAGE_USAGE_STORAGE_BIT;
	VkMemoryPropertyFlags properties;

	if ( flags.bTransferSrc == true )
	{
		usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	}

	if ( flags.bTransferDst == true )
	{
		usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	}

	if ( flags.bMappable == true )
	{
		properties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
	}
	else
	{
		properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	}
	
	// Possible extensions for below
	wsi_image_create_info wsiImageCreateInfo = {};
	VkExternalMemoryImageCreateInfo externalImageCreateInfo = {};
	VkImageDrmFormatModifierExplicitCreateInfoEXT modifierInfo = {};
	VkSubresourceLayout modifierPlaneLayouts[4] = {};
	VkImageDrmFormatModifierListCreateInfoEXT modifierListInfo = {};
	
	VkImageCreateInfo imageInfo = {};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.extent.width = width;
	imageInfo.extent.height = height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.format = format;
	imageInfo.tiling = tiling;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.usage = usage;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	if ( pDMA != nullptr )
	{
		assert( format == DRMFormatToVulkan( pDMA->format ) );
	}

	if ( g_vulkanSupportsModifiers && pDMA && pDMA->modifier != DRM_FORMAT_MOD_INVALID )
	{
		VkExternalImageFormatProperties externalImageProperties = {};
		externalImageProperties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
		res = getModifierProps( &imageInfo, pDMA->modifier, &externalImageProperties );
		if ( res != VK_SUCCESS && res != VK_ERROR_FORMAT_NOT_SUPPORTED ) {
			fprintf( stderr, "getModifierProps failed\n" );
			return false;
		}

		if ( res == VK_SUCCESS &&
		     ( externalImageProperties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT ) )
		{
			modifierInfo.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
			modifierInfo.pNext = imageInfo.pNext;
			modifierInfo.drmFormatModifier = pDMA->modifier;
			modifierInfo.drmFormatModifierPlaneCount = pDMA->n_planes;
			modifierInfo.pPlaneLayouts = modifierPlaneLayouts;

			for ( int i = 0; i < pDMA->n_planes; ++i )
			{
				modifierPlaneLayouts[i].offset = pDMA->offset[i];
				modifierPlaneLayouts[i].rowPitch = pDMA->stride[i];
			}

			imageInfo.pNext = &modifierInfo;

			imageInfo.tiling = tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
		}
	}

	std::vector<uint64_t> modifiers = {};
	if ( flags.bFlippable == true && g_vulkanSupportsModifiers && !pDMA )
	{
		uint32_t drmFormat = VulkanFormatToDRM( format );
		assert( drmFormat != DRM_FORMAT_INVALID );

		uint64_t linear = DRM_FORMAT_MOD_LINEAR;

		const uint64_t *possibleModifiers;
		size_t numPossibleModifiers;
		if ( flags.bLinear )
		{
			possibleModifiers = &linear;
			numPossibleModifiers = 1;
		}
		else
		{
			const struct wlr_drm_format *drmFormatDesc = wlr_drm_format_set_get( &g_DRM.plane_formats, drmFormat );
			assert( drmFormatDesc != nullptr );
			possibleModifiers = drmFormatDesc->modifiers;
			numPossibleModifiers = drmFormatDesc->len;
		}

		for ( size_t i = 0; i < numPossibleModifiers; i++ )
		{
			uint64_t modifier = possibleModifiers[i];

			VkExternalImageFormatProperties externalFormatProps = {};
			externalFormatProps.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
			res = getModifierProps( &imageInfo, modifier, &externalFormatProps );
			if ( res == VK_ERROR_FORMAT_NOT_SUPPORTED )
				continue;
			else if ( res != VK_SUCCESS ) {
				fprintf( stderr, "getModifierProps failed\n" );
				return false;
			}

			if ( !( externalFormatProps.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT ) )
				continue;

			modifiers.push_back( modifier );
		}

		assert( modifiers.size() > 0 );

		modifierListInfo.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
		modifierListInfo.pNext = imageInfo.pNext;
		modifierListInfo.pDrmFormatModifiers = modifiers.data();
		modifierListInfo.drmFormatModifierCount = modifiers.size();

		imageInfo.pNext = &modifierListInfo;

		imageInfo.tiling = tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	}

	if ( flags.bFlippable == true && tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT )
	{
		// We want to scan-out the image
		wsiImageCreateInfo.sType = VK_STRUCTURE_TYPE_WSI_IMAGE_CREATE_INFO_MESA;
		wsiImageCreateInfo.scanout = VK_TRUE;
		wsiImageCreateInfo.pNext = imageInfo.pNext;
		
		imageInfo.pNext = &wsiImageCreateInfo;
	}
	
	if ( pDMA != nullptr )
	{
		externalImageCreateInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
		externalImageCreateInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		externalImageCreateInfo.pNext = imageInfo.pNext;
		
		imageInfo.pNext = &externalImageCreateInfo;
	}
	
	m_format = imageInfo.format;

	if (vkCreateImage(device, &imageInfo, nullptr, &m_vkImage) != VK_SUCCESS) {
		fprintf( stderr, "vkCreateImage failed\n" );
		return false;
	}
	
	VkMemoryRequirements memRequirements;
	vkGetImageMemoryRequirements(device, m_vkImage, &memRequirements);
	
	// Possible pNexts
	wsi_memory_allocate_info wsiAllocInfo = {};
	VkImportMemoryFdInfoKHR importMemoryInfo = {};
	VkExportMemoryAllocateInfo memory_export_info = {};
	VkMemoryDedicatedAllocateInfo memory_dedicated_info = {};
	
	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;
	allocInfo.memoryTypeIndex = findMemoryType(properties, memRequirements.memoryTypeBits );
	
	if ( flags.bFlippable == true || pDMA != nullptr )
	{
		memory_dedicated_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
		memory_dedicated_info.image = m_vkImage;
		memory_dedicated_info.buffer = VK_NULL_HANDLE;
		memory_dedicated_info.pNext = allocInfo.pNext;
		
		allocInfo.pNext = &memory_dedicated_info;
	}
	
	if ( flags.bFlippable == true && pDMA == nullptr )
	{
		// We'll export it to DRM
		memory_export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
		memory_export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		memory_export_info.pNext = allocInfo.pNext;
		
		allocInfo.pNext = &memory_export_info;
	}
	
	if ( pDMA != nullptr )
	{
		// TODO: multi-planar DISTINCT DMA-BUFs support (see vkBindImageMemory2
		// and VkBindImagePlaneMemoryInfo)
		assert( allDMABUFsEqual( pDMA ) );

		// Importing memory from a FD transfers ownership of the FD
		int fd = dup( pDMA->fd[0] );
		if ( fd < 0 )
		{
			perror( "dup failed" );
			return false;
		}

		// We're importing WSI buffers from GL or Vulkan, set implicit_sync
		wsiAllocInfo.sType = VK_STRUCTURE_TYPE_WSI_MEMORY_ALLOCATE_INFO_MESA;
		wsiAllocInfo.implicit_sync = true;
		wsiAllocInfo.pNext = allocInfo.pNext;
		
		allocInfo.pNext = &wsiAllocInfo;
		
		// Memory already provided by pDMA
		importMemoryInfo.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
		importMemoryInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		importMemoryInfo.fd = fd;
		importMemoryInfo.pNext = allocInfo.pNext;
		
		allocInfo.pNext = &importMemoryInfo;
	}
	
	if (vkAllocateMemory(device, &allocInfo, nullptr, &m_vkImageMemory) != VK_SUCCESS) {
		return false;
	}
	
	res = vkBindImageMemory(device, m_vkImage, m_vkImageMemory, 0);
	if ( res != VK_SUCCESS ) {
		fprintf( stderr, "vkBindImageMemory failed\n" );
		return false;
	}

	if ( flags.bMappable == true )
	{
		assert( tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT );
		const VkImageSubresource image_subresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.mipLevel = 0,
			.arrayLayer = 0,
		};
		VkSubresourceLayout image_layout;
		vkGetImageSubresourceLayout(device, m_vkImage, &image_subresource, &image_layout);

		m_unRowPitch = image_layout.rowPitch;
	}
	
	if ( flags.bFlippable == true )
	{
		// We assume we own the memory when doing this right now.
		// We could support the import scenario as well if needed (but we
		// already have a DMA-BUF in that case).
// 		assert( bTextureable == false );
		assert( pDMA == nullptr );

		struct wlr_dmabuf_attributes dmabuf = {};
		dmabuf.width = width;
		dmabuf.height = height;
		dmabuf.format = VulkanFormatToDRM( format );
		assert( dmabuf.format != DRM_FORMAT_INVALID );

		// TODO: disjoint planes support
		const VkMemoryGetFdInfoKHR memory_get_fd_info = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
			.pNext = NULL,
			.memory = m_vkImageMemory,
			.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		};
		res = dyn_vkGetMemoryFdKHR(device, &memory_get_fd_info, &dmabuf.fd[0]);
		if ( res != VK_SUCCESS ) {
			fprintf( stderr, "vkGetMemoryFdKHR failed\n" );
			return false;
		}

		if ( tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT )
		{
			assert( dyn_vkGetImageDrmFormatModifierPropertiesEXT != nullptr );
			VkImageDrmFormatModifierPropertiesEXT imgModifierProps = {};
			imgModifierProps.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
			res = dyn_vkGetImageDrmFormatModifierPropertiesEXT( device, m_vkImage, &imgModifierProps );
			if ( res != VK_SUCCESS ) {
				fprintf( stderr, "vkGetImageDrmFormatModifierPropertiesEXT failed\n" );
				return false;
			}
			dmabuf.modifier = imgModifierProps.drmFormatModifier;

			assert( DRMModifierProps.count( format ) > 0);
			assert( DRMModifierProps[ format ].count( dmabuf.modifier ) > 0);

			dmabuf.n_planes = DRMModifierProps[ format ][ dmabuf.modifier ].drmFormatModifierPlaneCount;

			const VkImageAspectFlagBits planeAspects[] = {
				VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
				VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
				VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT,
				VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT,
			};
			assert( dmabuf.n_planes <= 4 );

			for ( int i = 0; i < dmabuf.n_planes; i++ )
			{
				const VkImageSubresource subresource = {
					.aspectMask = planeAspects[i],
					.mipLevel = 0,
					.arrayLayer = 0,
				};
				VkSubresourceLayout subresourceLayout = {};
				vkGetImageSubresourceLayout( device, m_vkImage, &subresource, &subresourceLayout );
				dmabuf.offset[i] = subresourceLayout.offset;
				dmabuf.stride[i] = subresourceLayout.rowPitch;
			}

			// Copy the first FD to all other planes
			for ( int i = 1; i < dmabuf.n_planes; i++ )
			{
				dmabuf.fd[i] = dup( dmabuf.fd[0] );
				if ( dmabuf.fd[i] < 0 ) {
					fprintf( stderr, "dup failed\n" );
					return false;
				}
			}
		}
		else
		{
			const VkImageSubresource subresource = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.mipLevel = 0,
				.arrayLayer = 0,
			};
			VkSubresourceLayout subresourceLayout = {};
			vkGetImageSubresourceLayout( device, m_vkImage, &subresource, &subresourceLayout );

			dmabuf.n_planes = 1;
			dmabuf.modifier = DRM_FORMAT_MOD_INVALID;
			dmabuf.offset[0] = 0;
			dmabuf.stride[0] = subresourceLayout.rowPitch;
		}

		m_FBID = drm_fbid_from_dmabuf( &g_DRM, nullptr, &dmabuf );
		if ( m_FBID == 0 ) {
			fprintf( stderr, "drm_fbid_from_dmabuf failed\n" );
			return false;
		}

		wlr_dmabuf_attributes_finish( &dmabuf );
	}

	bool bSwapChannels = pDMA ? DRMFormatNeedsSwizzle( pDMA->format ) : false;
	bool bHasAlpha = pDMA ? DRMFormatHasAlpha( pDMA->format ) : true;

	if ( bSwapChannels || !bHasAlpha )
	{
		// Right now this implies no storage bit - check it now as that's incompatible with swizzle
		assert ( flags.bTextureable == true );
	}
	
	VkImageViewCreateInfo createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	createInfo.image = m_vkImage;
	createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	createInfo.format = format;
	createInfo.components.r = bSwapChannels ? VK_COMPONENT_SWIZZLE_B : VK_COMPONENT_SWIZZLE_IDENTITY;
	createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
	createInfo.components.b = bSwapChannels ? VK_COMPONENT_SWIZZLE_R : VK_COMPONENT_SWIZZLE_IDENTITY;
// 	createInfo.components.a = bHasAlpha ? VK_COMPONENT_SWIZZLE_IDENTITY : VK_COMPONENT_SWIZZLE_ONE;
	createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
	createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	createInfo.subresourceRange.baseMipLevel = 0;
	createInfo.subresourceRange.levelCount = 1;
	createInfo.subresourceRange.baseArrayLayer = 0;
	createInfo.subresourceRange.layerCount = 1;
	
	res = vkCreateImageView(device, &createInfo, nullptr, &m_vkImageView);
	if ( res != VK_SUCCESS ) {
		fprintf( stderr, "vkCreateImageView failed\n" );
		return false;
	}

	if ( flags.bMappable )
	{
		vkMapMemory( device, g_output.pScreenshotImage->m_vkImageMemory, 0, VK_WHOLE_SIZE, 0, &m_pMappedData );

		if ( m_pMappedData == nullptr )
		{
			fprintf( stderr, "vkMapMemory failed\n" );
			return false;
		}
	}
	
	m_bInitialized = true;
	
	return true;
}

CVulkanTexture::~CVulkanTexture( void )
{
	if ( m_pMappedData != nullptr )
	{
		vkUnmapMemory( device, g_output.pScreenshotImage->m_vkImageMemory );
		m_pMappedData = nullptr;
	}

	if ( m_vkImageView != VK_NULL_HANDLE )
	{
		vkDestroyImageView( device, m_vkImageView, nullptr );
		m_vkImageView = VK_NULL_HANDLE;
	}

	if ( m_FBID != 0 )
	{
		drm_drop_fbid( &g_DRM, m_FBID );
		m_FBID = 0;
	}


	if ( m_vkImage != VK_NULL_HANDLE )
	{
		vkDestroyImage( device, m_vkImage, nullptr );
		m_vkImage = VK_NULL_HANDLE;
	}


	if ( m_vkImageMemory != VK_NULL_HANDLE )
	{
		vkFreeMemory( device, m_vkImageMemory, nullptr );
		m_vkImageMemory = VK_NULL_HANDLE;
	}

	m_bInitialized = false;
}

void init_formats()
{
	for ( size_t i = 0; s_DRMVKFormatTable[i].DRMFormat != DRM_FORMAT_INVALID; i++ )
	{
		VkFormat format = s_DRMVKFormatTable[i].vkFormat;
		uint32_t drmFormat = s_DRMVKFormatTable[i].DRMFormat;

		// First, check whether the Vulkan format is supported
		VkPhysicalDeviceImageFormatInfo2 imageFormatInfo = {};
		imageFormatInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
		imageFormatInfo.format = format;
		imageFormatInfo.type = VK_IMAGE_TYPE_2D;
		imageFormatInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageFormatInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
		imageFormatInfo.flags = 0;
		VkImageFormatProperties2 imageFormatProps = {};
		imageFormatProps.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
		VkResult res = vkGetPhysicalDeviceImageFormatProperties2( physicalDevice, &imageFormatInfo, &imageFormatProps );
		if ( res == VK_ERROR_FORMAT_NOT_SUPPORTED )
		{
			continue;
		}
		else if ( res != VK_SUCCESS )
		{
			fprintf( stderr, "vkGetPhysicalDeviceImageFormatProperties2 failed for DRM format 0x%" PRIX32 "\n", drmFormat );
			continue;
		}

		sampledShmFormats.push_back( drmFormat );

		if ( !g_vulkanSupportsModifiers )
		{
			if ( BIsNested() == false && !wlr_drm_format_set_has( &g_DRM.formats, drmFormat, DRM_FORMAT_MOD_INVALID ) )
			{
				continue;
			}
			wlr_drm_format_set_add( &sampledDRMFormats, drmFormat, DRM_FORMAT_MOD_INVALID );
			continue;
		}

		// Then, collect the list of modifiers supported for sampled usage
		VkDrmFormatModifierPropertiesListEXT modifierPropList = {};
		modifierPropList.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
		VkFormatProperties2 formatProps = {};
		formatProps.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
		formatProps.pNext = &modifierPropList;
		vkGetPhysicalDeviceFormatProperties2( physicalDevice, format, &formatProps );

		if ( modifierPropList.drmFormatModifierCount == 0 )
		{
			fprintf( stderr, "vkGetPhysicalDeviceFormatProperties2 returned zero modifiers for DRM format 0x%" PRIX32 "\n", drmFormat );
			continue;
		}

		std::vector<VkDrmFormatModifierPropertiesEXT> modifierProps(modifierPropList.drmFormatModifierCount);
		modifierPropList.pDrmFormatModifierProperties = modifierProps.data();
		vkGetPhysicalDeviceFormatProperties2( physicalDevice, format, &formatProps );

		std::map< uint64_t, VkDrmFormatModifierPropertiesEXT > map = {};

		for ( size_t j = 0; j < modifierProps.size(); j++ )
		{
			map[ modifierProps[j].drmFormatModifier ] = modifierProps[j];

			uint64_t modifier = modifierProps[j].drmFormatModifier;

			if ( ( modifierProps[j].drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT ) == 0 )
			{
				continue;
			}
			if ( BIsNested() == false && !wlr_drm_format_set_has( &g_DRM.formats, drmFormat, modifier ) )
			{
				continue;
			}
			if ( BIsNested() == false && drmFormat == DRM_FORMAT_NV12 && modifier == DRM_FORMAT_MOD_LINEAR && g_bRotated )
			{
				// If embedded and rotated, blacklist NV12 LINEAR because
				// amdgpu won't support direct scan-out. Since only pure
				// Wayland clients can submit NV12 buffers, this should only
				// affect streaming_client.
				continue;
			}
			wlr_drm_format_set_add( &sampledDRMFormats, drmFormat, modifier );
		}

		DRMModifierProps[ format ] = map;
	}

	fprintf( stderr, "Supported DRM formats for sampling usage: " );
	for ( size_t i = 0; i < sampledDRMFormats.len; i++ )
	{
		uint32_t fmt = sampledDRMFormats.formats[ i ]->format;
		fprintf( stderr, "%s0x%" PRIX32, i == 0 ? "" : ", ", fmt );
	}
	fprintf( stderr, "\n" );
}

static bool is_vulkan_1_1_device(VkPhysicalDevice device)
{
	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(device, &properties);

	return properties.apiVersion >= VK_API_VERSION_1_1;
}

int init_device()
{
	uint32_t physicalDeviceCount = 0;
	VkPhysicalDevice deviceHandles[MAX_DEVICE_COUNT];
	VkQueueFamilyProperties queueFamilyProperties[MAX_QUEUE_COUNT];
	
	vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, 0);
	physicalDeviceCount = physicalDeviceCount > MAX_DEVICE_COUNT ? MAX_DEVICE_COUNT : physicalDeviceCount;
	vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, deviceHandles);

	bool bTryComputeOnly = true;

	// In theory vkBasalt might want to filter out compute-only queue families to force our hand here
	const char *pchEnableVkBasalt = getenv( "ENABLE_VKBASALT" );
	if ( pchEnableVkBasalt != nullptr && pchEnableVkBasalt[0] == '1' )
	{
		bTryComputeOnly = false;
	}
	
retry:
	for (uint32_t i = 0; i < physicalDeviceCount; ++i)
	{
		if (!is_vulkan_1_1_device(deviceHandles[i]))
			continue;

		uint32_t queueFamilyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(deviceHandles[i], &queueFamilyCount, NULL);
		queueFamilyCount = queueFamilyCount > MAX_QUEUE_COUNT ? MAX_QUEUE_COUNT : queueFamilyCount;
		vkGetPhysicalDeviceQueueFamilyProperties(deviceHandles[i], &queueFamilyCount, queueFamilyProperties);
		
		for (uint32_t j = 0; j < queueFamilyCount; ++j) {
			if ( queueFamilyProperties[j].queueFlags & VK_QUEUE_COMPUTE_BIT &&
				( bTryComputeOnly == false || !(queueFamilyProperties[j].queueFlags & VK_QUEUE_GRAPHICS_BIT ) ) &&
				( bTryComputeOnly == true || queueFamilyProperties[j].queueFlags & VK_QUEUE_GRAPHICS_BIT ) )
			{
				queueFamilyIndex = j;
				physicalDevice = deviceHandles[i];
			}
		}
		
		if (physicalDevice)
		{
			break;
		}
	}

	if ( bTryComputeOnly == true && physicalDevice == VK_NULL_HANDLE )
	{
		bTryComputeOnly = false;
		goto retry;
	}

	if (!physicalDevice)
	{
		fprintf(stderr, "Failed to find physical device\n");
		return false;
	}
	
	vkGetPhysicalDeviceMemoryProperties( physicalDevice, &memoryProperties );

	uint32_t supportedExtensionCount;
	vkEnumerateDeviceExtensionProperties( physicalDevice, NULL, &supportedExtensionCount, NULL );

	std::vector<VkExtensionProperties> vecSupportedExtensions(supportedExtensionCount);
	vkEnumerateDeviceExtensionProperties( physicalDevice, NULL, &supportedExtensionCount, vecSupportedExtensions.data() );

	g_vulkanSupportsModifiers = false;
	for ( uint32_t i = 0; i < supportedExtensionCount; ++i )
	{
		if ( strcmp(vecSupportedExtensions[i].extensionName,
		     VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) == 0 )
			g_vulkanSupportsModifiers = true;
	}

	fprintf( stderr, "Vulkan %s DRM format modifiers\n",
			g_vulkanSupportsModifiers ? "supports" : "does not support" );

	init_formats();

	float queuePriorities = 1.0f;

	VkDeviceQueueGlobalPriorityCreateInfoEXT queueCreateInfoEXT = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_EXT,
		.pNext = nullptr,
		.globalPriority = VK_QUEUE_GLOBAL_PRIORITY_REALTIME_EXT
	};
	
	VkDeviceQueueCreateInfo queueCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.pNext = nullptr,
		.queueFamilyIndex = queueFamilyIndex,
		.queueCount = 1,
		.pQueuePriorities = &queuePriorities
	};

	if ( g_bNiceCap == true )
	{
		queueCreateInfo.pNext = &queueCreateInfoEXT;
	}
	
	std::vector< const char * > vecEnabledDeviceExtensions;
	
	if ( BIsNested() == true )
	{
		vecEnabledDeviceExtensions.push_back( VK_KHR_SWAPCHAIN_EXTENSION_NAME );
	}

	if ( g_vulkanSupportsModifiers )
	{
		vecEnabledDeviceExtensions.push_back( VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME );
	}

	vecEnabledDeviceExtensions.push_back( VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME );
	vecEnabledDeviceExtensions.push_back( VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME );

	vecEnabledDeviceExtensions.push_back( VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME );
	
	VkDeviceCreateInfo deviceCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queueCreateInfo,
		.enabledLayerCount = 0,
		.ppEnabledLayerNames = 0,
		.enabledExtensionCount = (uint32_t)vecEnabledDeviceExtensions.size(),
		.ppEnabledExtensionNames = vecEnabledDeviceExtensions.data(),
		.pEnabledFeatures = 0,
	};

	VkResult result = vkCreateDevice(physicalDevice, &deviceCreateInfo, NULL, &device);
	
	if ( result != VK_SUCCESS )
	{
		return false;
	}
	
	vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);
	
	if ( queue == VK_NULL_HANDLE )
		return false;
	
	VkShaderModuleCreateInfo shaderModuleCreateInfo = {};
	shaderModuleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shaderModuleCreateInfo.codeSize = sizeof(composite_spv);
	shaderModuleCreateInfo.pCode = (const uint32_t*)composite_spv;
	
	VkResult res = vkCreateShaderModule( device, &shaderModuleCreateInfo, nullptr, &shaderModule );
	
	if ( res != VK_SUCCESS )
	{
		return false;
	}
	
	VkCommandPoolCreateInfo commandPoolCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.pNext = nullptr,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = queueFamilyIndex,
	};

	res = vkCreateCommandPool(device, &commandPoolCreateInfo, 0, &commandPool);
	
	if ( res != VK_SUCCESS )
	{
		return false;
	}

	VkDescriptorPoolSize descriptorPoolSize[] = {
		{
			VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			k_nMaxSets * 1,
		},
		{
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			k_nMaxSets * 1,
		},
		{
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			2 * k_nMaxSets * k_nMaxLayers,
		},
	};
	
	VkDescriptorPoolCreateInfo descriptorPoolCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.maxSets = k_nMaxSets,
		.poolSizeCount = sizeof(descriptorPoolSize) / sizeof(descriptorPoolSize[0]),
		.pPoolSizes = descriptorPoolSize
	};
	
	res = vkCreateDescriptorPool(device, &descriptorPoolCreateInfo, 0, &descriptorPool);
	
	if ( res != VK_SUCCESS )
	{
		return false;
	}

	VkFormatProperties nv12Properties;
	vkGetPhysicalDeviceFormatProperties(physicalDevice, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, &nv12Properties);
	bool cosited = nv12Properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT;

	VkSamplerYcbcrConversionCreateInfo ycbcrSamplerConversionCreateInfo = 
	{
		.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
		.pNext = nullptr,
		.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
		.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
		.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW,
		.xChromaOffset = cosited ? VK_CHROMA_LOCATION_COSITED_EVEN : VK_CHROMA_LOCATION_MIDPOINT,
		.yChromaOffset = cosited ? VK_CHROMA_LOCATION_COSITED_EVEN : VK_CHROMA_LOCATION_MIDPOINT,
		.chromaFilter = VK_FILTER_LINEAR,
		.forceExplicitReconstruction = VK_FALSE,
	};

	VkSamplerYcbcrConversion ycbcrConversion;
	vkCreateSamplerYcbcrConversion( device, &ycbcrSamplerConversionCreateInfo, nullptr, &ycbcrConversion );

	VkSampler ycbcrSampler = VK_NULL_HANDLE;

	VkSamplerYcbcrConversionInfo ycbcrSamplerConversionInfo = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
		.pNext = nullptr,
		.conversion = ycbcrConversion,
	};

	VkSamplerCreateInfo ycbcrSamplerInfo = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.pNext = &ycbcrSamplerConversionInfo,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		// Using clamp to edge here for now, otherwise we end up with a
		// 1/2 pixel of green on the left and top of the screen due to
		// the texel replacement happening before the YCbCr conversion.

		// TODO: Find out why we are even hitting the border in the first place,
		// maybe there is some weird half-texel stuff somewhere in Gamescope causing this,
		// I imagine if this was more obvious it would affect games too.
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
		.unnormalizedCoordinates = VK_TRUE,
	};
	
	vkCreateSampler( device, &ycbcrSamplerInfo, nullptr, &ycbcrSampler );

	std::array<VkSampler, k_nMaxLayers> ycbcrSamplers = {ycbcrSampler, ycbcrSampler, ycbcrSampler, ycbcrSampler};
	
	std::vector< VkDescriptorSetLayoutBinding > vecLayoutBindings;
	VkDescriptorSetLayoutBinding descriptorSetLayoutBindings =
	{
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	};
	
	vecLayoutBindings.push_back( descriptorSetLayoutBindings ); // first binding is target storage image
	
	descriptorSetLayoutBindings.binding = 1;
	descriptorSetLayoutBindings.descriptorCount = 1;
	descriptorSetLayoutBindings.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	
	vecLayoutBindings.push_back( descriptorSetLayoutBindings ); // second binding is composite description buffer
	
	descriptorSetLayoutBindings.binding = 2;
	descriptorSetLayoutBindings.descriptorCount = k_nMaxLayers;
	descriptorSetLayoutBindings.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

	vecLayoutBindings.push_back( descriptorSetLayoutBindings );

	descriptorSetLayoutBindings.binding = 6;
	descriptorSetLayoutBindings.descriptorCount = k_nMaxLayers;
	descriptorSetLayoutBindings.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	descriptorSetLayoutBindings.pImmutableSamplers = ycbcrSamplers.data();

	vecLayoutBindings.push_back( descriptorSetLayoutBindings );
	
	VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo =
	{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.bindingCount = (uint32_t)vecLayoutBindings.size(),
		.pBindings = vecLayoutBindings.data()
	};
	
	res = vkCreateDescriptorSetLayout(device, &descriptorSetLayoutCreateInfo, 0, &descriptorSetLayout);
	
	if ( res != VK_SUCCESS )
	{
		return false;
	}
	
	VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {
		VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		0,
		0,
		1,
		&descriptorSetLayout,
		0,
		0
	};
	
	res = vkCreatePipelineLayout(device, &pipelineLayoutCreateInfo, 0, &pipelineLayout);
	
	if ( res != VK_SUCCESS )
	{
		return false;
	}

	const std::array<VkSpecializationMapEntry, 3> specializationEntries = {{
		{
			.constantID = 0,
			.offset     = 0,
			.size       = sizeof(uint32_t)
		},
		{
			.constantID = 1,
			.offset     = sizeof(uint32_t),
			.size       = sizeof(VkBool32)
		},
		{
			.constantID = 2,
			.offset     = sizeof(uint32_t) + sizeof(uint32_t),
			.size       = sizeof(uint32_t)
		},
	}};
	
	for (uint32_t layerCount = 0; layerCount < k_nMaxLayers; layerCount++) {
		for (VkBool32 swapChannels = 0; swapChannels < 2; swapChannels++) {
			for (uint32_t ycbcrMask = 0; ycbcrMask < k_nMaxYcbcrMask; ycbcrMask++) {
				struct {
					uint32_t layerCount;
					VkBool32 swapChannels;
					uint32_t ycbcrMask;
				} specializationData = {
					.layerCount   = layerCount + 1,
					.swapChannels = swapChannels,
					.ycbcrMask    = ycbcrMask,
				};

				VkSpecializationInfo specializationInfo = {
					.mapEntryCount = uint32_t(specializationEntries.size()),
					.pMapEntries   = specializationEntries.data(),
					.dataSize      = sizeof(specializationData),
					.pData		   = &specializationData,
				};

				VkComputePipelineCreateInfo computePipelineCreateInfo = {
					VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
					0,
					0,
					{
						VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
						0,
						0,
						VK_SHADER_STAGE_COMPUTE_BIT,
						shaderModule,
						"main",
						&specializationInfo
					},
					pipelineLayout,
					0,
					0
				};

				res = vkCreateComputePipelines(device, 0, 1, &computePipelineCreateInfo, 0, &pipelines[layerCount][swapChannels][ycbcrMask]);
				if (res != VK_SUCCESS)
					return false;
			}
		}
	}
	
	VkDescriptorSetAllocateInfo descriptorSetAllocateInfo = {
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		nullptr,
		descriptorPool,
		1,
		&descriptorSetLayout
	};
	
	res = vkAllocateDescriptorSets(device, &descriptorSetAllocateInfo, &descriptorSet);
	
	if ( res != VK_SUCCESS || descriptorSet == VK_NULL_HANDLE )
	{
		return false;
	}
	
	// Make and map upload buffer
	
	VkBufferCreateInfo bufferCreateInfo = {};
	bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferCreateInfo.pNext = nullptr;
	bufferCreateInfo.size = 512 * 512 * 4;
	bufferCreateInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	
	result = vkCreateBuffer( device, &bufferCreateInfo, nullptr, &uploadBuffer );
	
	if ( result != VK_SUCCESS )
	{
		return false;
	}
	
	VkMemoryRequirements memRequirements;
	vkGetBufferMemoryRequirements(device, uploadBuffer, &memRequirements);
	
	int memTypeIndex =  findMemoryType(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, memRequirements.memoryTypeBits );
	
	if ( memTypeIndex == -1 )
	{
		return false;
	}
	
	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;
	allocInfo.memoryTypeIndex = memTypeIndex;
	
	vkAllocateMemory( device, &allocInfo, nullptr, &uploadBufferMemory);
	
	vkBindBufferMemory( device, uploadBuffer, uploadBufferMemory, 0 );
	vkMapMemory( device, uploadBufferMemory, 0, VK_WHOLE_SIZE, 0, (void**)&pUploadBuffer );
	
	if ( pUploadBuffer == nullptr )
	{
		return false;
	}
	
	VkCommandBufferAllocateInfo commandBufferAllocateInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.pNext = nullptr,
		.commandPool = commandPool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1
	};
	
	for ( uint32_t i = 0; i < k_nScratchCmdBufferCount; i++ )
	{
		result = vkAllocateCommandBuffers( device, &commandBufferAllocateInfo, &g_scratchCommandBuffers[ i ].cmdBuf );
		
		if ( result != VK_SUCCESS )
		{
			return false;
		}
		
		VkFenceCreateInfo fenceCreateInfo =
		{
			.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
		};
		
		result = vkCreateFence( device, &fenceCreateInfo, nullptr, &g_scratchCommandBuffers[ i ].fence );
		
		if ( result != VK_SUCCESS )
		{
			return false;
		}
		
		g_scratchCommandBuffers[ i ].busy = false;
	}
	
	
	return true;
}

void fini_device()
{
	vkDestroyDevice(device, 0);
}

bool acquire_next_image( void )
{
	VkResult res = vkAcquireNextImageKHR( device, g_output.swapChain, UINT64_MAX, VK_NULL_HANDLE, VK_NULL_HANDLE, &g_output.nSwapChainImageIndex );
	return res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR;
}

void vulkan_present_to_window( void )
{
	VkPresentInfoKHR presentInfo = {};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	
// 	presentInfo.waitSemaphoreCount = 1;
// 	presentInfo.pWaitSemaphores = signalSemaphores;
	
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = &g_output.swapChain;
	
	presentInfo.pImageIndices = &g_output.nSwapChainImageIndex;
	
	if ( vkQueuePresentKHR( queue, &presentInfo ) != VK_SUCCESS )
		vulkan_remake_swapchain();
	
	while ( !acquire_next_image() )
		vulkan_remake_swapchain();
}

bool vulkan_make_swapchain( VulkanOutput_t *pOutput )
{
	uint32_t imageCount = pOutput->surfaceCaps.minImageCount + 1;
	uint32_t surfaceFormat = 0;
	uint32_t formatCount = pOutput->surfaceFormats.size();
	VkResult result = VK_SUCCESS;

	for ( surfaceFormat = 0; surfaceFormat < formatCount; surfaceFormat++ )
	{
		if ( pOutput->surfaceFormats[ surfaceFormat ].format == VK_FORMAT_B8G8R8A8_UNORM )
			break;
	}
	
	if ( surfaceFormat == formatCount )
		return false;

	pOutput->outputFormat = pOutput->surfaceFormats[ surfaceFormat ].format;
	
	VkSwapchainCreateInfoKHR createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	createInfo.surface = pOutput->surface;
	
	createInfo.minImageCount = imageCount;
	createInfo.imageFormat = pOutput->outputFormat;
	createInfo.imageColorSpace = pOutput->surfaceFormats[surfaceFormat ].colorSpace;
	createInfo.imageExtent = { g_nOutputWidth, g_nOutputHeight };
	createInfo.imageArrayLayers = 1;
	createInfo.imageUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	
	createInfo.preTransform = pOutput->surfaceCaps.currentTransform;
	createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	createInfo.clipped = VK_TRUE;
	
	createInfo.oldSwapchain = VK_NULL_HANDLE;
	
	if (vkCreateSwapchainKHR( device, &createInfo, nullptr, &pOutput->swapChain) != VK_SUCCESS ) {
		return 0;
	}
	
	vkGetSwapchainImagesKHR( device, pOutput->swapChain, &imageCount, nullptr );
	pOutput->swapChainImages.resize( imageCount );
	pOutput->swapChainImageViews.resize( imageCount );
	vkGetSwapchainImagesKHR( device, pOutput->swapChain, &imageCount, pOutput->swapChainImages.data() );
	
	for ( uint32_t i = 0; i < pOutput->swapChainImages.size(); i++ )
	{
		VkImageViewCreateInfo createInfo = {};
		createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		createInfo.image = pOutput->swapChainImages[ i ];
		createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		createInfo.format = pOutput->outputFormat;
		createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
		createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
		createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
		createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
		createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		createInfo.subresourceRange.baseMipLevel = 0;
		createInfo.subresourceRange.levelCount = 1;
		createInfo.subresourceRange.baseArrayLayer = 0;
		createInfo.subresourceRange.layerCount = 1;
		
		result = vkCreateImageView(device, &createInfo, nullptr, &pOutput->swapChainImageViews[ i ]);
		
		if ( result != VK_SUCCESS )
			return false;
	}
	
	return true;
}

bool vulkan_remake_swapchain( void )
{
	VulkanOutput_t *pOutput = &g_output;
	vkQueueWaitIdle( queue );

	for ( uint32_t i = 0; i < pOutput->swapChainImages.size(); i++ )
	{
		vkDestroyImageView( device, pOutput->swapChainImageViews[ i ], nullptr );
		
		pOutput->swapChainImageViews[ i ] = VK_NULL_HANDLE;
		pOutput->swapChainImages[ i ] = VK_NULL_HANDLE;
	}
	
	vkDestroySwapchainKHR( device, pOutput->swapChain, nullptr );
	
	pOutput->nSwapChainImageIndex = 0;

	// Delete screenshot image to be remade if needed
	if ( pOutput->pScreenshotImage != nullptr )
	{
		delete pOutput->pScreenshotImage;
		pOutput->pScreenshotImage = nullptr;
	}
	
	bool bRet = vulkan_make_swapchain( &g_output );
	assert( bRet ); // Something has gone horribly wrong!
	return bRet;
}

bool vulkan_make_output( VulkanOutput_t *pOutput )
{
	VkResult result;
	
	if ( BIsNested() == true )
	{
		if ( !SDL_Vulkan_CreateSurface( g_SDLWindow, instance, &pOutput->surface ) )
		{
			fprintf( stderr, "SDL_Vulkan_CreateSurface failed\n" );
			return false;
		}

		// TODO: check this when selecting the physical device and queue family
		VkBool32 canPresent = false;
		vkGetPhysicalDeviceSurfaceSupportKHR( physicalDevice, queueFamilyIndex, pOutput->surface, &canPresent );
		if ( !canPresent )
		{
			fprintf( stderr, "Physical device queue doesn't support presenting on our surface\n" );
			return false;
		}

		result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR( physicalDevice, pOutput->surface, &pOutput->surfaceCaps );
		
		if ( result != VK_SUCCESS )
			return false;
		
		uint32_t formatCount = 0;
		result = vkGetPhysicalDeviceSurfaceFormatsKHR( physicalDevice, pOutput->surface, &formatCount, nullptr );
		
		if ( result != VK_SUCCESS )
			return false;
		
		if ( formatCount != 0 ) {
			pOutput->surfaceFormats.resize( formatCount );
			vkGetPhysicalDeviceSurfaceFormatsKHR( physicalDevice, pOutput->surface, &formatCount, pOutput->surfaceFormats.data() );
			
			if ( result != VK_SUCCESS )
				return false;
		}
		
		uint32_t presentModeCount = false;
		result = vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, pOutput->surface, &presentModeCount, nullptr );
		
		if ( result != VK_SUCCESS )
			return false;
		
		if ( presentModeCount != 0 ) {
			pOutput->presentModes.resize(presentModeCount);
			result = vkGetPhysicalDeviceSurfacePresentModesKHR( physicalDevice, pOutput->surface, &presentModeCount, pOutput->presentModes.data() );
			
			if ( result != VK_SUCCESS )
				return false;
		}
		
		if ( !vulkan_make_swapchain( pOutput ) )
			return false;

		while ( !acquire_next_image() )
			vulkan_remake_swapchain();
	}
	else
	{
		pOutput->outputFormat = DRMFormatToVulkan( g_nDRMFormat );
		
		if ( pOutput->outputFormat == VK_FORMAT_UNDEFINED )
		{
			fprintf( stderr, "Failed to find Vulkan format suitable for KMS\n" );
			return false;
		}

		CVulkanTexture::createFlags outputImageflags;
		outputImageflags.bFlippable = true;
		outputImageflags.bTransferSrc = true; // for screenshots
		
		bool bSuccess = pOutput->outputImage[0].BInit( g_nOutputWidth, g_nOutputHeight, pOutput->outputFormat, outputImageflags );
		
		if ( bSuccess != true )
		{
			fprintf( stderr, "Failed to allocate buffer for KMS\n" );
			return false;
		}
		
		bSuccess = pOutput->outputImage[1].BInit( g_nOutputWidth, g_nOutputHeight, pOutput->outputFormat, outputImageflags );
		
		if ( bSuccess != true )
			return false;
	}

	// Make and map constant buffer
	
	VkBufferCreateInfo bufferCreateInfo = {};
	bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferCreateInfo.pNext = nullptr;
	bufferCreateInfo.size = sizeof( Composite_t::CompositeData_t );
	bufferCreateInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
	
	result = vkCreateBuffer( device, &bufferCreateInfo, nullptr, &pOutput->constantBuffer );
	
	if ( result != VK_SUCCESS )
	{
		return false;
	}
	
	VkMemoryRequirements memRequirements;
	vkGetBufferMemoryRequirements(device, pOutput->constantBuffer, &memRequirements);
	
	int memTypeIndex =  findMemoryType(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, memRequirements.memoryTypeBits );
	
	if ( memTypeIndex == -1 )
	{
		return false;
	}
	
	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;
	allocInfo.memoryTypeIndex = memTypeIndex;
	
	vkAllocateMemory( device, &allocInfo, nullptr, &pOutput->bufferMemory );
	
	vkBindBufferMemory( device, pOutput->constantBuffer, pOutput->bufferMemory, 0 );
	vkMapMemory( device, pOutput->bufferMemory, 0, VK_WHOLE_SIZE, 0, (void**)&pOutput->pCompositeBuffer );
	
	if ( pOutput->pCompositeBuffer == nullptr )
	{
		return false;
	}
	
	// Make command buffers
	
	VkCommandBufferAllocateInfo commandBufferAllocateInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.pNext = nullptr,
		.commandPool = commandPool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 2
	};
	
	result = vkAllocateCommandBuffers(device, &commandBufferAllocateInfo, pOutput->commandBuffers);
	
	if ( result != VK_SUCCESS )
	{
		return false;
	}
	
	pOutput->nCurCmdBuffer = 0;
	
	pOutput->fence = VK_NULL_HANDLE;
	pOutput->fenceFD = -1;
	
	// Write the constant buffer itno descriptor set
	VkDescriptorBufferInfo bufferInfo = {
		.buffer = g_output.constantBuffer,
		.offset = 0,
		.range = VK_WHOLE_SIZE
	};
	
	VkWriteDescriptorSet writeDescriptorSet = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.pNext = nullptr,
		.dstSet = descriptorSet,
		.dstBinding = 1,
		.dstArrayElement = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.pImageInfo = nullptr,
		.pBufferInfo = &bufferInfo,
		.pTexelBufferView = nullptr,
	};
	
	vkUpdateDescriptorSets(device, 1, &writeDescriptorSet, 0, nullptr);
	
	return true;
}

int vulkan_init(void)
{
	VkResult result = VK_ERROR_INITIALIZATION_FAILED;
	
	std::vector< const char * > vecEnabledInstanceExtensions;
	vecEnabledInstanceExtensions.insert( vecEnabledInstanceExtensions.end(), g_vecSDLInstanceExts.begin(), g_vecSDLInstanceExts.end() );
	
	const VkInstanceCreateInfo createInfo = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &appInfo,
		.enabledExtensionCount = (uint32_t)vecEnabledInstanceExtensions.size(),
		.ppEnabledExtensionNames = vecEnabledInstanceExtensions.data(),
	};

	result = vkCreateInstance(&createInfo, 0, &instance);
	
	if ( result != VK_SUCCESS )
		return 0;
	
	if ( init_device() != 1 )
	{
		return 0;
	}
	
	dyn_vkGetMemoryFdKHR = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr( device, "vkGetMemoryFdKHR" );
	if ( dyn_vkGetMemoryFdKHR == nullptr )
		return 0;
	
	dyn_vkGetFenceFdKHR = (PFN_vkGetFenceFdKHR)vkGetDeviceProcAddr( device, "vkGetFenceFdKHR" );
	if ( dyn_vkGetFenceFdKHR == nullptr )
		return 0;

	dyn_vkGetImageDrmFormatModifierPropertiesEXT = (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr( device, "vkGetImageDrmFormatModifierPropertiesEXT" );
	
	if ( vulkan_make_output( &g_output ) == false )
	{
		fprintf( stderr, "vulkan_make_output failed\n" );
		return 0;
	}

	uint32_t bits = 0;
	g_emptyTex = vulkan_create_texture_from_bits( 1, 1, VK_FORMAT_R8G8B8A8_UNORM, &bits );

	if ( g_emptyTex == 0 )
	{
		return 0;
	}
	
	return 1;
}

static inline uint32_t get_command_buffer( VkCommandBuffer &cmdBuf, VkFence *pFence )
{
	for ( uint32_t i = 0; i < k_nScratchCmdBufferCount; i++ )
	{
		if ( g_scratchCommandBuffers[ i ].busy == false )
		{
			cmdBuf = g_scratchCommandBuffers[ i ].cmdBuf;
			if ( pFence != nullptr )
			{
				*pFence = g_scratchCommandBuffers[ i ].fence;
			}
			
			VkCommandBufferBeginInfo commandBufferBeginInfo = {
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
			};
			
			VkResult res = vkBeginCommandBuffer( cmdBuf, &commandBufferBeginInfo);
			
			if ( res != VK_SUCCESS )
			{
				break;
			}
			
			g_scratchCommandBuffers[ i ].refs.clear();
			
			g_scratchCommandBuffers[ i ].haswaiter = pFence != nullptr;
			
			g_scratchCommandBuffers[ i ].busy = true;
			
			return i;
		}
	}
	
	assert( 0 );
	return 0;
}

static inline void submit_command_buffer( uint32_t handle, std::vector<CVulkanTexture *> &vecRefs )
{
	VkCommandBuffer cmdBuf = g_scratchCommandBuffers[ handle ].cmdBuf;
	VkFence fence = g_scratchCommandBuffers[ handle ].fence;
	
	VkResult res = vkEndCommandBuffer( cmdBuf );
	
	if ( res != VK_SUCCESS )
	{
		assert( 0 );
	}
	
	VkSubmitInfo submitInfo = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		. commandBufferCount = 1,
		.pCommandBuffers = &cmdBuf,
	};
	
	res = vkQueueSubmit( queue, 1, &submitInfo, fence );
	
	if ( res != VK_SUCCESS )
	{
		assert( 0 );
	}
	
	for( uint32_t i = 0; i < vecRefs.size(); i++ )
	{
		vecRefs[ i ]->nRefCount++;
		g_scratchCommandBuffers[ handle ].refs.push_back( vecRefs[ i ] );
	}
}

void vulkan_garbage_collect( void )
{
	// If we ever made anything calling get_command_buffer() multi-threaded we'd have to rethink this
	// Probably just differentiate "busy" and "submitted"
	for ( uint32_t i = 0; i < k_nScratchCmdBufferCount; i++ )
	{
		if ( g_scratchCommandBuffers[ i ].busy == true &&
			g_scratchCommandBuffers[ i ].haswaiter == false )
		{
			VkResult res = vkGetFenceStatus( device, g_scratchCommandBuffers[ i ].fence );
			
			if ( res == VK_SUCCESS )
			{
				vkResetCommandBuffer( g_scratchCommandBuffers[ i ].cmdBuf, 0 );
				vkResetFences( device, 1, &g_scratchCommandBuffers[ i ].fence );
				
				for ( uint32_t ref = 0; ref < g_scratchCommandBuffers[ i ].refs.size(); ref++ )
				{
					CVulkanTexture *pTex = g_scratchCommandBuffers[ i ].refs[ ref ];
					pTex->nRefCount--;
					
					if ( pTex->nRefCount == 0 )
					{
						g_mapVulkanTextures[ pTex->handle ] = nullptr;
						delete pTex;
					}
				}

				g_scratchCommandBuffers[ i ].busy = false;
			}
		}
	}
}

VulkanTexture_t vulkan_create_texture_from_dmabuf( struct wlr_dmabuf_attributes *pDMA )
{
	VulkanTexture_t ret = 0;

	CVulkanTexture *pTex = new CVulkanTexture();

	CVulkanTexture::createFlags texCreateFlags;
	texCreateFlags.bTextureable = true;
	
	if ( pTex->BInit( pDMA->width, pDMA->height, DRMFormatToVulkan( pDMA->format ), texCreateFlags, pDMA ) == false )
	{
		delete pTex;
		return ret;
	}
	
	ret = ++g_nMaxVulkanTexHandle;
	g_mapVulkanTextures[ ret ] = pTex;
	
	pTex->handle = ret;
	
	return ret;
}

VulkanTexture_t vulkan_create_texture_from_bits( uint32_t width, uint32_t height, VkFormat format, void *bits )
{
	VulkanTexture_t ret = 0;
	
	CVulkanTexture *pTex = new CVulkanTexture();

	CVulkanTexture::createFlags texCreateFlags;
	texCreateFlags.bFlippable = BIsNested() == false;
	texCreateFlags.bLinear = true; // cursor buffer needs to be linear
	texCreateFlags.bTextureable = true;
	texCreateFlags.bTransferDst = true;
	
	if ( pTex->BInit( width, height, format, texCreateFlags ) == false )
	{
		delete pTex;
		return ret;
	}
	
	memcpy( pUploadBuffer, bits, width * height * 4 );
	
	VkCommandBuffer commandBuffer;
	uint32_t handle = get_command_buffer( commandBuffer, nullptr );
	
	VkBufferImageCopy region = {};
	
	region.imageSubresource = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.layerCount = 1
	};
	
	region.imageExtent = {
		.width = width,
		.height = height,
		.depth = 1
	};
	
	vkCmdCopyBufferToImage( commandBuffer, uploadBuffer, pTex->m_vkImage, VK_IMAGE_LAYOUT_GENERAL, 1, &region );
	
	std::vector<CVulkanTexture *> refs;
	refs.push_back( pTex );
	
	submit_command_buffer( handle, refs );
	
	ret = ++g_nMaxVulkanTexHandle;
	g_mapVulkanTextures[ ret ] = pTex;
	
	pTex->handle = ret;
	
	return ret;
}

void vulkan_free_texture( VulkanTexture_t vulkanTex )
{
	if ( vulkanTex == 0 )
		return;
	
	CVulkanTexture *pTex = g_mapVulkanTextures[ vulkanTex ];

	assert( pTex != nullptr );
	assert( pTex->handle == vulkanTex );
	
	pTex->nRefCount--;
	
	if ( pTex->nRefCount == 0 )
	{
		delete pTex;
		g_mapVulkanTextures[ vulkanTex ] = nullptr;
	}
}

bool operator==(const struct VulkanPipeline_t::LayerBinding_t& lhs, struct VulkanPipeline_t::LayerBinding_t& rhs)
{
	if ( lhs.bFilter != rhs.bFilter )
		return false;

	if ( lhs.bBlackBorder != rhs.bBlackBorder )
		return false;

	return true;
}

VkSampler vulkan_make_sampler( struct VulkanPipeline_t::LayerBinding_t *pBinding )
{
	VkSampler ret = VK_NULL_HANDLE;

	VkSamplerCreateInfo samplerCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.pNext = nullptr,
		.magFilter = pBinding->bFilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
		.minFilter = pBinding->bFilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
		.borderColor = pBinding->bBlackBorder ? VK_BORDER_COLOR_INT_OPAQUE_BLACK : VK_BORDER_COLOR_INT_TRANSPARENT_BLACK,
		.unnormalizedCoordinates = VK_TRUE,
	};
	
	vkCreateSampler( device, &samplerCreateInfo, nullptr, &ret );
	
	return ret;
}

void vulkan_update_descriptor( struct VulkanPipeline_t *pPipeline, int nYCBCRMask )
{
	{
		VkImageView targetImageView;
		
		if ( BIsNested() == true )
		{
			targetImageView = g_output.swapChainImageViews[ g_output.nSwapChainImageIndex ];
		}
		else
		{
			targetImageView = g_output.outputImage[ g_output.nOutImage ].m_vkImageView;
		}

		VkDescriptorImageInfo imageInfo = {
			.imageView = targetImageView,
			.imageLayout = VK_IMAGE_LAYOUT_GENERAL
		};
		
		VkWriteDescriptorSet writeDescriptorSet = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.pNext = nullptr,
			.dstSet = descriptorSet,
			.dstBinding = 0,
			.dstArrayElement = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			.pImageInfo = &imageInfo,
			.pBufferInfo = nullptr,
			.pTexelBufferView = nullptr,
		};
		
		vkUpdateDescriptorSets(device, 1, &writeDescriptorSet, 0, nullptr);
	}
	
	std::array< VkDescriptorImageInfo, k_nMaxLayers > imageDescriptors;
	for ( uint32_t i = 0; i < k_nMaxLayers; i++ )
	{
		VkSampler sampler = VK_NULL_HANDLE;
		CVulkanTexture *pTex = nullptr;

		if ( pPipeline->layerBindings[ i ].tex != 0 )
		{
			pTex = g_mapVulkanTextures[ pPipeline->layerBindings[ i ].tex ];
		}
		else
		{
			pTex = g_mapVulkanTextures[ g_emptyTex ];

			// Switch the border to transparent black
			pPipeline->layerBindings[ i ].bBlackBorder = false;
		}
		
		// First try to look up the sampler in the cache.
		for ( uint32_t j = 0; j < g_vecVulkanSamplerCache.size(); j++ )
		{
			if ( g_vecVulkanSamplerCache[ j ].key == pPipeline->layerBindings[ i ] )
			{
				sampler = g_vecVulkanSamplerCache[ j ].sampler;
				break;
			}
		}
		
		if ( sampler == VK_NULL_HANDLE )
		{
			sampler = vulkan_make_sampler( &pPipeline->layerBindings[ i ] );
			
			assert( sampler != VK_NULL_HANDLE );
			
			VulkanSamplerCacheEntry_t entry = { pPipeline->layerBindings[ i ], sampler };
			g_vecVulkanSamplerCache.push_back( entry );
		}

		{
			VkDescriptorImageInfo imageInfo = {
				.sampler = sampler,
				.imageView = pTex->m_vkImageView,
				.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
			};

			imageDescriptors[ i ] = imageInfo;
		}
	}

	std::array< VkWriteDescriptorSet, 2 > writeDescriptorSets;

	writeDescriptorSets[0] = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.pNext = nullptr,
		.dstSet = descriptorSet,
		.dstBinding = 2,
		.dstArrayElement = 0,
		.descriptorCount = imageDescriptors.size(),
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = imageDescriptors.data(),
		.pBufferInfo = nullptr,
		.pTexelBufferView = nullptr,
	};

	if ( nYCBCRMask != 0 )
	{
		// Duplicate image descriptors for ycbcr.
		std::array< VkDescriptorImageInfo, k_nMaxLayers > ycbcrImageDescriptors;
		for (uint32_t i = 0; i < k_nMaxLayers; i++)
		{
			ycbcrImageDescriptors[i] = imageDescriptors[i];
			// We use immutable samplers.
			ycbcrImageDescriptors[i].sampler = VK_NULL_HANDLE;
		}

		writeDescriptorSets[1] = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.pNext = nullptr,
			.dstSet = descriptorSet,
			.dstBinding = 6,
			.dstArrayElement = 0,
			.descriptorCount = ycbcrImageDescriptors.size(),
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = ycbcrImageDescriptors.data(),
			.pBufferInfo = nullptr,
			.pTexelBufferView = nullptr,
		};

		vkUpdateDescriptorSets(device, 2, writeDescriptorSets.data(), 0, nullptr);
	}
	else
	{
		vkUpdateDescriptorSets(device, 1, writeDescriptorSets.data(), 0, nullptr);
	}
}

bool vulkan_composite( struct Composite_t *pComposite, struct VulkanPipeline_t *pPipeline, bool bScreenshot )
{
	VkImage compositeImage;

	if ( BIsNested() == true )
	{
		compositeImage = g_output.swapChainImages[ g_output.nSwapChainImageIndex ];
	}
	else
	{
		compositeImage = g_output.outputImage[ g_output.nOutImage ].m_vkImage;

		if ( DRMFormatNeedsSwizzle( g_nDRMFormat ) )
		{
			pComposite->nSwapChannels = 1;
		}
	}
	
	pComposite->nYCBCRMask = 0;
	for (uint32_t i = 0; i < k_nMaxLayers; i++)
	{
		if ( pPipeline->layerBindings[ i ].tex != 0 )
		{
			CVulkanTexture *pTex = g_mapVulkanTextures[ pPipeline->layerBindings[ i ].tex ];
			if (pTex->m_format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)
				pComposite->nYCBCRMask |= 1 << i;
		}
	}

	// Sample a bit closer to texel centers in most cases
	// TODO: probably actually need to apply a general scale/bias to properly
	// sample from the center in all four corners in all scaling scenarios
	for ( int i = 0; i < pComposite->nLayerCount; i++ )
	{
		pComposite->data.vOffset[ i ].x += 0.5f;
		pComposite->data.vOffset[ i ].y += 0.5f;
	}

	*g_output.pCompositeBuffer = pComposite->data;
	// XXX maybe flush something?
	
	assert ( g_output.fence == VK_NULL_HANDLE );
	
	vulkan_update_descriptor( pPipeline, pComposite->nYCBCRMask );
	
	VkCommandBuffer curCommandBuffer = g_output.commandBuffers[ g_output.nCurCmdBuffer ];
	
	VkCommandBufferBeginInfo commandBufferBeginInfo = {
		VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		0,
		VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		0
	};
	
	VkResult res = vkResetCommandBuffer( curCommandBuffer, 0 );
	
	if ( res != VK_SUCCESS )
	{
		return false;
	}
	
	res = vkBeginCommandBuffer( curCommandBuffer, &commandBufferBeginInfo);
	
	if ( res != VK_SUCCESS )
	{
		return false;
	}
	
	vkCmdBindPipeline(curCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[pComposite->nLayerCount - 1][pComposite->nSwapChannels][pComposite->nYCBCRMask]);
	
	vkCmdBindDescriptorSets(curCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
							pipelineLayout, 0, 1, &descriptorSet, 0, 0);
	
	uint32_t nGroupCountX = currentOutputWidth % 8 ? currentOutputWidth / 8 + 1: currentOutputWidth / 8;
	uint32_t nGroupCountY = currentOutputHeight % 8 ? currentOutputHeight / 8 + 1: currentOutputHeight / 8;
	
	vkCmdDispatch( curCommandBuffer, nGroupCountX, nGroupCountY, 1 );
	
	// Pipeline barrier to flush our compute operations so the buffer can be scanned out safely
	VkImageSubresourceRange subResRange =
	{
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.levelCount = 1,
		.layerCount = 1
	};

	VkImageMemoryBarrier memoryBarrier =
	{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT, // ?
		.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL, // does it flush more to transntion to PRESENT_SRC?
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = compositeImage,
		.subresourceRange = subResRange
	};
	
	vkCmdPipelineBarrier( curCommandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
						  0, 0, nullptr, 0, nullptr, 1, &memoryBarrier );

	if ( bScreenshot == true )
	{
		if ( g_output.pScreenshotImage == nullptr )
		{
			g_output.pScreenshotImage = new CVulkanTexture;

			CVulkanTexture::createFlags screenshotImageFlags;
			screenshotImageFlags.bMappable = true;
			screenshotImageFlags.bTransferDst = true;

			bool bSuccess = g_output.pScreenshotImage->BInit( currentOutputWidth, currentOutputHeight, g_output.outputFormat, screenshotImageFlags );

			assert( bSuccess );

			// Transition it to GENERAL
			VkImageSubresourceRange subResRange =
			{
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.levelCount = 1,
				.layerCount = 1
			};

			VkImageMemoryBarrier memoryBarrier =
			{
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				.srcAccessMask = 0,
				.dstAccessMask = 0,
				.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				.newLayout = VK_IMAGE_LAYOUT_GENERAL,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.image = g_output.pScreenshotImage->m_vkImage,
				.subresourceRange = subResRange
			};

			vkCmdPipelineBarrier( curCommandBuffer, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
								  0, 0, nullptr, 0, nullptr, 1, &memoryBarrier );
		}

		VkImageCopy region = {};

		region.srcSubresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.layerCount = 1
		};

		region.dstSubresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.layerCount = 1
		};

		region.extent = {
			.width = currentOutputWidth,
			.height = currentOutputHeight,
			.depth = 1
		};

		vkCmdCopyImage( curCommandBuffer, compositeImage, VK_IMAGE_LAYOUT_GENERAL, g_output.pScreenshotImage->m_vkImage, VK_IMAGE_LAYOUT_GENERAL, 1, &region );
	}
	
	res = vkEndCommandBuffer( curCommandBuffer );
	
	if ( res != VK_SUCCESS )
	{
		return false;
	}
	
// 	VkExportFenceCreateInfoKHR exportFenceCreateInfo = 
// 	{
// 		.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO,
// 		.handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT_KHR
// 	};
// 	
// 	VkFenceCreateInfo fenceCreateInfo =
// 	{
// 		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
// 		.pNext = &exportFenceCreateInfo,
// 	};
// 	
// 	res = vkCreateFence( device, &fenceCreateInfo, nullptr, &g_output.fence );
// 	
// 	if ( res != VK_SUCCESS )
// 	{
// 		return false;
// 	}
// 	
// 	assert( g_output.fence != VK_NULL_HANDLE );
// 	
// 	VkFenceGetFdInfoKHR fenceGetFDInfo =
// 	{
// 		.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR,
// 		.fence = g_output.fence,
// 		.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT_KHR
// 	};
// 	
// 	res = dyn_vkGetFenceFdKHR( device, &fenceGetFDInfo, &g_output.fenceFD );
// 	
// 	if ( res != VK_SUCCESS )
// 	{
// 		return false;
// 	}
// 	
// 	// In theory it can start as -1 if it's already signaled, but there's no way in this case
// 	assert( g_output.fenceFD != -1 );
	
	VkSubmitInfo submitInfo = {
		VK_STRUCTURE_TYPE_SUBMIT_INFO,
		0,
		0,
		0,
		0,
		1,
		&curCommandBuffer,
		0,
		0
	};
	
	res = vkQueueSubmit( queue, 1, &submitInfo, 0 );
	
	if ( res != VK_SUCCESS )
	{
		return false;
	}
	
	vkQueueWaitIdle( queue );

	if ( bScreenshot )
	{
		uint32_t redMask = 0x00ff0000;
		uint32_t greenMask = 0x0000ff00;
		uint32_t blueMask = 0x000000ff;
		uint32_t alphaMask = 0;

		SDL_Surface *pSDLSurface = SDL_CreateRGBSurfaceFrom( g_output.pScreenshotImage->m_pMappedData, currentOutputWidth, currentOutputHeight, 32,
															 g_output.pScreenshotImage->m_unRowPitch, redMask, greenMask, blueMask, alphaMask );

		static char pTimeBuffer[1024];

		time_t currentTime = time(0);
		struct tm *localTime = localtime( &currentTime );
		strftime( pTimeBuffer, sizeof( pTimeBuffer ), "/tmp/gamescope_%Y-%m-%d_%H-%M-%S.bmp", localTime );

		SDL_SaveBMP( pSDLSurface, pTimeBuffer );

		SDL_FreeSurface( pSDLSurface );
	}
	
	if ( BIsNested() == false )
	{
		g_output.nOutImage = !g_output.nOutImage;
	}
	
	g_output.nCurCmdBuffer = !g_output.nCurCmdBuffer;
	
	return true;
}

uint32_t vulkan_get_last_composite_fbid( void )
{
	return g_output.outputImage[ !g_output.nOutImage ].m_FBID;
}

uint32_t vulkan_texture_get_fbid( VulkanTexture_t vulkanTex )
{
	if ( vulkanTex == 0 )
		return 0;
	
	assert( g_mapVulkanTextures[ vulkanTex ] != nullptr );
	
	uint32_t ret = g_mapVulkanTextures[ vulkanTex ]->m_FBID;
	
	assert( ret != 0 );
	
	return ret;
}

static void texture_destroy( struct wlr_texture *wlr_texture )
{
	VulkanWlrTexture_t *tex = (VulkanWlrTexture_t *)wlr_texture;
	wlr_buffer_unlock( tex->buf );
	delete tex;
}

static const struct wlr_texture_impl texture_impl = {
	.destroy = texture_destroy,
};

static uint32_t renderer_get_render_buffer_caps( struct wlr_renderer *renderer )
{
	return 0;
}

static void renderer_begin( struct wlr_renderer *renderer, uint32_t width, uint32_t height )
{
	abort(); // unreachable
}

static void renderer_end( struct wlr_renderer *renderer )
{
	abort(); // unreachable
}

static void renderer_clear( struct wlr_renderer *renderer, const float color[4] )
{
	abort(); // unreachable
}

static void renderer_scissor( struct wlr_renderer *renderer, struct wlr_box *box )
{
	abort(); // unreachable
}

static bool renderer_render_subtexture_with_matrix( struct wlr_renderer *renderer, struct wlr_texture *texture, const struct wlr_fbox *box, const float matrix[9], float alpha )
{
	abort(); // unreachable
}

static void renderer_render_quad_with_matrix( struct wlr_renderer *renderer, const float color[4], const float matrix[9] )
{
	abort(); // unreachable
}

static const uint32_t *renderer_get_shm_texture_formats( struct wlr_renderer *wlr_renderer, size_t *len
 )
{
	*len = sampledShmFormats.size();
	return sampledShmFormats.data();
}

static const struct wlr_drm_format_set *renderer_get_dmabuf_texture_formats( struct wlr_renderer *wlr_renderer )
{
	return &sampledDRMFormats;
}

static struct wlr_texture *renderer_texture_from_buffer( struct wlr_renderer *wlr_renderer, struct wlr_buffer *buf )
{
	VulkanWlrTexture_t *tex = new VulkanWlrTexture_t();
	wlr_texture_init( &tex->base, &texture_impl, buf->width, buf->height );
	tex->buf = wlr_buffer_lock( buf );
	// TODO: check format/modifier
	// TODO: if DMA-BUF, try importing it into Vulkan
	return &tex->base;
}

static const struct wlr_renderer_impl renderer_impl = {
	.begin = renderer_begin,
	.end = renderer_end,
	.clear = renderer_clear,
	.scissor = renderer_scissor,
	.render_subtexture_with_matrix = renderer_render_subtexture_with_matrix,
	.render_quad_with_matrix = renderer_render_quad_with_matrix,
	.get_shm_texture_formats = renderer_get_shm_texture_formats,
	.get_dmabuf_texture_formats = renderer_get_dmabuf_texture_formats,
	.get_render_buffer_caps = renderer_get_render_buffer_caps,
	.texture_from_buffer = renderer_texture_from_buffer,
};

struct wlr_renderer *vulkan_renderer_create( struct wlr_renderer *parent )
{
	VulkanRenderer_t *renderer = new VulkanRenderer_t();
	wlr_renderer_init(&renderer->base, &renderer_impl);
	renderer->parent = parent;
	return &renderer->base;
}

VulkanTexture_t vulkan_create_texture_from_wlr_buffer( struct wlr_buffer *buf )
{

	struct wlr_dmabuf_attributes dmabuf = {0};
	if ( wlr_buffer_get_dmabuf( buf, &dmabuf ) )
	{
		return vulkan_create_texture_from_dmabuf( &dmabuf );
	}

	if ( !buf->impl->begin_data_ptr_access )
	{
		return 0;
	}

	VkResult result;

	void *src;
	uint32_t drmFormat;
	size_t stride;
	if ( !buf->impl->begin_data_ptr_access( buf, &src, &drmFormat, &stride ) )
	{
		return 0;
	}

	VkFormat format = DRMFormatToVulkan( drmFormat );
	uint32_t width = buf->width;
	uint32_t height = buf->height;

	VkBufferCreateInfo bufferCreateInfo = {};
	bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferCreateInfo.size = stride * height;
	bufferCreateInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	VkBuffer buffer;
	result = vkCreateBuffer( device, &bufferCreateInfo, nullptr, &buffer );
	if ( result != VK_SUCCESS )
	{
		buf->impl->end_data_ptr_access( buf );
		return 0;
	}

	VkMemoryRequirements memRequirements;
	vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

	int memTypeIndex =  findMemoryType(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, memRequirements.memoryTypeBits );
	if ( memTypeIndex == -1 )
	{
		buf->impl->end_data_ptr_access( buf );
		return 0;
	}

	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;
	allocInfo.memoryTypeIndex = memTypeIndex;

	VkDeviceMemory bufferMemory;
	result = vkAllocateMemory( device, &allocInfo, nullptr, &bufferMemory);
	if ( result != VK_SUCCESS )
	{
		buf->impl->end_data_ptr_access( buf );
		return 0;
	}

	result = vkBindBufferMemory( device, buffer, bufferMemory, 0 );
	if ( result != VK_SUCCESS )
	{
		buf->impl->end_data_ptr_access( buf );
		return 0;
	}

	void *dst;
	result = vkMapMemory( device, bufferMemory, 0, VK_WHOLE_SIZE, 0, &dst );
	if ( result != VK_SUCCESS )
	{
		buf->impl->end_data_ptr_access( buf );
		return 0;
	}

	memcpy( dst, src, stride * height );

	vkUnmapMemory( device, bufferMemory );

	buf->impl->end_data_ptr_access( buf );

	CVulkanTexture *pTex = new CVulkanTexture();
	CVulkanTexture::createFlags texCreateFlags = {};
	//texCreateFlags.bFlippable = BIsNested() == false;
	texCreateFlags.bTextureable = true;
	texCreateFlags.bTransferDst = true;
	if ( pTex->BInit( width, height, format, texCreateFlags ) == false )
	{
		delete pTex;
		return 0;
	}

	VkCommandBuffer commandBuffer;
	uint32_t handle = get_command_buffer( commandBuffer, nullptr );

	VkBufferImageCopy region = {};
	region.imageSubresource = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.layerCount = 1
	};
	region.imageExtent = {
		.width = width,
		.height = height,
		.depth = 1
	};
	vkCmdCopyBufferToImage( commandBuffer, buffer, pTex->m_vkImage, VK_IMAGE_LAYOUT_GENERAL, 1, &region );

	std::vector<CVulkanTexture *> refs;
	refs.push_back( pTex );

	submit_command_buffer( handle, refs );

	VulkanTexture_t texid = ++g_nMaxVulkanTexHandle;
	pTex->handle = texid;
	g_mapVulkanTextures[ texid ] = pTex;

	return texid;
}
