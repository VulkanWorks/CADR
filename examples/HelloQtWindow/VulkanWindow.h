#pragma once

#include <QWindow>
#ifdef _WIN32
# define VK_USE_PLATFORM_WIN32_KHR
#else
# define VK_USE_PLATFORM_XLIB_KHR
#endif
#include <vulkan/vulkan.hpp>


#if QT_VERSION>=0x050a00 // include QVulkanInstance for Qt 5.10 and newer or provide alternative
#include <QVulkanInstance>
#else
class QVulkanInstance {
protected:
	vk::Instance _instance;
	bool _owned;
public:
	inline VkInstance vkInstance() const  { return _instance; }
	void setVkInstance(VkInstance existingVkInstance)  { destroy(); _instance=existingVkInstance; _owned=false; }
	void destroy()  { if(_instance && _owned) { _instance.destroy(); _instance=nullptr; } }
};
#endif


class VulkanWindow : public QWindow {
public:
	typedef void ExceptionHandler(const std::exception* e);
protected:
	typedef QWindow inherited;

	ExceptionHandler* _exceptionHandler = defaultExceptionHandler;

#if QT_VERSION<0x050a00 // provide missing methods for Qt 5.9 and earlier
	QVulkanInstance* _vulkanInstance;  ///< Vulkan instance used by the window. The instance must not be destroyed before the window is destroyed.
#endif

	// Vulkan handles and objects
	// (they need to be placed in particular (not arbitrary) order as it gives their destruction order)
	vk::UniqueSurfaceKHR _surface;
	vk::PhysicalDevice physicalDevice;
	uint32_t graphicsQueueFamily;
	uint32_t presentationQueueFamily;
	vk::UniqueDevice device;
	vk::Queue graphicsQueue;
	vk::Queue presentationQueue;
	vk::SurfaceFormatKHR chosenSurfaceFormat;
	vk::Format depthFormat;
	vk::UniqueRenderPass renderPass;
	vk::UniqueShaderModule vsModule;
	vk::UniqueShaderModule fsModule;
	vk::UniquePipelineCache pipelineCache;
	vk::UniquePipelineLayout pipelineLayout;
	vk::Extent2D _currentSurfaceExtent;
	vk::UniqueSwapchainKHR swapchain;
	std::vector<vk::UniqueImageView> swapchainImageViews;
	vk::UniqueImage depthImage;
	vk::UniqueDeviceMemory depthImageMemory;
	vk::UniqueImageView depthImageView;
	vk::UniquePipeline pipeline;
	std::vector<vk::UniqueFramebuffer> framebuffers;
	vk::UniqueCommandPool commandPool;
	std::vector<vk::UniqueCommandBuffer> commandBuffers;
	vk::UniqueSemaphore imageAvailableSemaphore;
	vk::UniqueSemaphore renderFinishedSemaphore;

public:

	virtual ~VulkanWindow()  {}
	static void defaultExceptionHandler(const std::exception* e);

	#if QT_VERSION<0x050a00 // provide missing methods for Qt 5.9 and earlier
	inline QVulkanInstance* vulkanInstance() const  { return _vulkanInstance; }
	inline void setVulkanInstance(QVulkanInstance* instance)  { _vulkanInstance=instance; }
#endif

protected:
	virtual bool event(QEvent* e) override;
	virtual void showEvent(QShowEvent* e) override;
	virtual void resizeEvent(QResizeEvent* e) override;
	virtual void exposeEvent(QExposeEvent* e) override;
};


// inline methods
