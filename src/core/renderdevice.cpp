/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 Xaver Hugl <xaver.hugl@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "renderdevice.h"

#include "drmdevice.h"
#include "graphicsbuffer.h"
#include "opengl/eglcontext.h"
#include "opengl/egldisplay.h"
#include "utils/common.h"
#include "utils/envvar.h"
#include "vulkan/vulkan_device.h"
#include "vulkan/vulkan_logging.h"

#include <expected>
#include <vulkan/vulkan_raii.hpp>
#include <vulkan/vulkan_to_string.hpp>
#if __has_include(<sys/sysmacros.h>)
#include <sys/sysmacros.h>
#endif

namespace KWin
{

static const bool s_disableVulkan = environmentVariableBoolValue("KWIN_DISABLE_VULKAN").value_or(false);
// Diagnostic override for comparing async compute against running the same
// compositor workload on the graphics queue. The default remains to prefer a
// compute-only queue.
static const bool s_forceVulkanGraphicsQueue = environmentVariableBoolValue("KWIN_VULKAN_FORCE_GRAPHICS_QUEUE").value_or(false);

// NOTE that we have to create an instance per render device, as Mesa
// only updates the list of devices on the first vkEnumeratePhysicalDevices
// call per VkInstance!
static vk::raii::Instance createVulkanInstance(const vk::raii::Context &context)
{
    if (s_disableVulkan) {
        return nullptr;
    }
    vk::ApplicationInfo appInfo{
        "kwin_wayland",
        VK_MAKE_VERSION(PROJECT_VERSION_MAJOR, PROJECT_VERSION_MINOR, PROJECT_VERSION_PATCH),
        "kwin_wayland",
        VK_MAKE_VERSION(1, 3, 0),
        VK_MAKE_VERSION(1, 3, 0),
    };
    std::vector<const char *> validationLayers;
    if (environmentVariableBoolValue("KWIN_VULKAN_VALIDATION").value_or(PROJECT_VERSION_PATCH >= 80)) {
        validationLayers.push_back("VK_LAYER_KHRONOS_validation");
    }
    vk::InstanceCreateInfo instanceInfo{
        vk::InstanceCreateFlags(),
        &appInfo,
        validationLayers,
    };
    auto [result, instance] = context.createInstance(instanceInfo);
    if (result != vk::Result::eSuccess && !validationLayers.empty()) {
        // try again without the validation layer
        validationLayers.clear();
        instanceInfo.setPEnabledLayerNames(validationLayers);
        auto [result, instance] = context.createInstance(instanceInfo);
        if (result == vk::Result::eSuccess) {
            qCWarning(KWIN_CORE, "Vulkan validation layer is not installed");
            return std::move(instance);
        }
    }
    return std::move(instance);
}

class RenderDevicePrivate
{
public:
    RenderDevicePrivate()
        : instance(createVulkanInstance(context))
    {
    }

    vk::raii::Context context;
    vk::raii::Instance instance;
};

static FormatModifierMap getImportFormats(EglDisplay *eglDisplay, VulkanDevice *vulkanDevice)
{
    FormatModifierMap ret;
    if (eglDisplay) {
        ret = eglDisplay->allSupportedDrmFormats();
    }
    if (vulkanDevice) {
        ret = ret.merged(vulkanDevice->supportedFormats());
    }
    return ret;
}

RenderDevice::RenderDevice(std::unique_ptr<DrmDevice> &&device, std::unique_ptr<EglDisplay> &&display)
    : m_device(std::move(device))
    , m_display(std::move(display))
    , m_vulkan(std::make_unique<RenderDevicePrivate>())
{
    createVulkanDevice();
    m_allImportableFormats = getImportFormats(m_display.get(), m_vulkanDevice.get());
}

RenderDevice::~RenderDevice()
{
}

DrmDevice *RenderDevice::drmDevice() const
{
    return m_device.get();
}

EglDisplay *RenderDevice::eglDisplay() const
{
    return m_display.get();
}

std::shared_ptr<EglContext> RenderDevice::eglContext(EglContext *shareContext)
{
    auto ret = m_eglContext.lock();
    if (!ret || ret->isFailed()) {
        ret = EglContext::create(m_display.get(), EGL_NO_CONFIG_KHR, shareContext);
        m_eglContext = ret;
    }
    return ret;
}

VulkanDevice *RenderDevice::vulkanDevice() const
{
    return m_vulkanDevice.get();
}

const FormatModifierMap &RenderDevice::allImportableFormats() const
{
    return m_allImportableFormats;
}

static constexpr std::array s_requiredVulkanExtensions = {
    // allows getting the dev_t of each VkPhysicalDevice
    VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME,
    // allow importing dma-bufs
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    // allow importing and exporting sync fds
    VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
};

static std::unique_ptr<VulkanDevice> openVulkanDevice(const vk::raii::Instance &instance, DrmDevice *drm)
{
    const auto [enumerateResult, physicalDevices] = instance.enumeratePhysicalDevices();
    if (enumerateResult != vk::Result::eSuccess) {
        qCWarning(KWIN_VULKAN) << "querying vulkan devices failed:" << vk::to_string(enumerateResult);
        return nullptr;
    }
#if HAVE_LIBDRM_FAUX
    // with faux devices like vkms and vgem, we can only do software rendering
    const bool needsSoftwareDevice = drm->busType() == DRM_BUS_FAUX;
#else
    const bool needsSoftwareDevice = false;
#endif
    for (const vk::raii::PhysicalDevice &physicalDevice : physicalDevices) {
        const auto basicProperties = physicalDevice.getProperties2();
        const bool isSoftwareDevice = basicProperties.properties.deviceType == vk::PhysicalDeviceType::eCpu;
        const char *deviceName = basicProperties.properties.deviceName.data();
        if (isSoftwareDevice != needsSoftwareDevice) {
            continue;
        }

        const auto [extensionPropResult, extensionProps] = physicalDevice.enumerateDeviceExtensionProperties();
        if (extensionPropResult != vk::Result::eSuccess) {
            continue;
        }
        std::vector usedExtensions = s_requiredVulkanExtensions | std::ranges::to<std::vector>();
        if (isSoftwareDevice) {
            // software devices don't usually have a render node
            std::erase(usedExtensions, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
        }
        std::vector missingExtensions = usedExtensions;
        std::erase_if(missingExtensions, [&extensionProps](std::string_view required) {
            return std::ranges::any_of(extensionProps, [required](const auto &ext) {
                return required == ext.extensionName;
            });
        });
        if (!missingExtensions.empty()) {
            qCWarning(KWIN_VULKAN, "Device %s misses required Vulkan extensions", deviceName);
            for (const char *str : missingExtensions) {
                qCWarning(KWIN_VULKAN) << str;
            }
            continue;
        }
        const auto fenceProperties = physicalDevice.getExternalFenceProperties(vk::PhysicalDeviceExternalFenceInfo{
            vk::ExternalFenceHandleTypeFlagBits::eSyncFd,
        });
        if (!(fenceProperties.externalFenceFeatures & vk::ExternalFenceFeatureFlagBits::eExportable)) {
            qCWarning(KWIN_VULKAN, "Vulkan device %s can't export sync fds", deviceName);
            continue;
        }

        if (!isSoftwareDevice) {
            const auto propertiesChain = physicalDevice.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDrmPropertiesEXT>();
            const auto &drmProps = propertiesChain.get<vk::PhysicalDeviceDrmPropertiesEXT>();
            if (!drmProps.hasRender) {
                qCDebug(KWIN_VULKAN, "Skipping device %s without a render node", deviceName);
                continue;
            }
            if (drm->deviceId() != makedev(drmProps.renderMajor, drmProps.renderMinor)) {
                continue;
            }
        }
        std::vector<vk::QueueFamilyProperties> queueProperties = physicalDevice.getQueueFamilyProperties();
        const bool hasGraphics = std::ranges::any_of(queueProperties, [](const vk::QueueFamilyProperties &props) {
            return bool(props.queueFlags & vk::QueueFlagBits::eGraphics);
        });
        const bool hasCompute = std::ranges::any_of(queueProperties, [](const vk::QueueFamilyProperties &props) {
            return bool(props.queueFlags & vk::QueueFlagBits::eCompute);
        });
        if (!hasGraphics || !hasCompute) {
            qCWarning(KWIN_VULKAN, "Physical device %s has no graphics or compute queue", deviceName);
            continue;
        }
        const auto supportedFeatureChain = physicalDevice.getFeatures2<vk::PhysicalDeviceFeatures2,
                                                                       vk::PhysicalDeviceHostQueryResetFeatures>();
        const vk::PhysicalDeviceFeatures supportedFeatures = supportedFeatureChain.get<vk::PhysicalDeviceFeatures2>().features;
        const bool supportsHostQueryReset = supportedFeatureChain.get<vk::PhysicalDeviceHostQueryResetFeatures>().hostQueryReset;
        if (!supportedFeatures.shaderStorageImageWriteWithoutFormat) {
            qCWarning(KWIN_VULKAN, "Physical device %s can't write storage images without a format", deviceName);
            continue;
        }
        if (!supportedFeatures.shaderStorageImageReadWithoutFormat) {
            qCWarning(KWIN_VULKAN, "Physical device %s can't read storage images without a format", deviceName);
            continue;
        }
        if (!supportedFeatures.shaderSampledImageArrayDynamicIndexing) {
            qCWarning(KWIN_VULKAN, "Physical device %s doesn't support dynamic sampled-image array indexing", deviceName);
            continue;
        }

        const auto graphicsIt = std::ranges::find_if(queueProperties, [](const vk::QueueFamilyProperties &props) {
            return bool(props.queueFlags & vk::QueueFlagBits::eGraphics);
        });
        auto computeIt = s_forceVulkanGraphicsQueue && (graphicsIt->queueFlags & vk::QueueFlagBits::eCompute)
            ? graphicsIt
            : std::ranges::find_if(queueProperties, [](const vk::QueueFamilyProperties &props) {
            return (props.queueFlags & vk::QueueFlagBits::eCompute) && !(props.queueFlags & vk::QueueFlagBits::eGraphics);
        });
        if (computeIt == queueProperties.end()) {
            computeIt = std::ranges::find_if(queueProperties, [](const vk::QueueFamilyProperties &props) {
                return bool(props.queueFlags & vk::QueueFlagBits::eCompute);
            });
        }
        const uint32_t computeQueueFamily = std::distance(queueProperties.begin(), computeIt);

        const auto supportsExtension = [&extensionProps](std::string_view name) {
            return std::ranges::any_of(extensionProps, [name](const vk::ExtensionProperties &extension) {
                return std::string_view(extension.extensionName.data()) == name;
            });
        };
        const char *globalPriorityExtensionName = nullptr;
        if (supportsExtension(VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME)) {
            globalPriorityExtensionName = VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME;
        } else if (supportsExtension(VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME)) {
            globalPriorityExtensionName = VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME;
        }
        const bool supportsGlobalPriority = globalPriorityExtensionName;
        if (supportsGlobalPriority) {
            usedExtensions.push_back(globalPriorityExtensionName);
        }

        std::vector<VkDeviceQueueCreateInfo> queueInfo;
        std::vector<VkDeviceQueueGlobalPriorityCreateInfoKHR> priorityInfo(queueProperties.size());
        float priority = 1;
        for (uint32_t i = 0; i < queueProperties.size(); i++) {
            if (supportsGlobalPriority && i == computeQueueFamily) {
                priorityInfo[i] = VkDeviceQueueGlobalPriorityCreateInfoKHR{
                    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR,
                    .pNext = nullptr,
                    .globalPriority = VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR,
                };
            }
            queueInfo.push_back(VkDeviceQueueCreateInfo{
                .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                .pNext = supportsGlobalPriority && i == computeQueueFamily ? &priorityInfo[i] : nullptr,
                .flags = {},
                .queueFamilyIndex = i,
                .queueCount = 1,
                .pQueuePriorities = &priority,
            });
        }

        vk::PhysicalDeviceHostQueryResetFeatures hostQueryResetFeatures;
        hostQueryResetFeatures.hostQueryReset = supportsHostQueryReset;
        vk::PhysicalDeviceSynchronization2Features syncFeatures;
        syncFeatures.pNext = &hostQueryResetFeatures;
        syncFeatures.synchronization2 = true;
        VkPhysicalDeviceFeatures features{
            .robustBufferAccess = true,
            .shaderStorageImageReadWithoutFormat = true,
            .shaderStorageImageWriteWithoutFormat = true,
            .shaderSampledImageArrayDynamicIndexing = true,
        };
        VkDeviceCreateInfo deviceInfo{
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext = &syncFeatures,
            .flags = {},
            .queueCreateInfoCount = uint32_t(queueInfo.size()),
            .pQueueCreateInfos = queueInfo.data(),
            .enabledLayerCount = 0,
            .ppEnabledLayerNames = nullptr,
            .enabledExtensionCount = uint32_t(usedExtensions.size()),
            .ppEnabledExtensionNames = usedExtensions.data(),
            .pEnabledFeatures = &features,
        };

        auto [result, logicalDevice] = physicalDevice.createDevice(deviceInfo);
        bool hasHighPriorityComputeQueue = supportsGlobalPriority && result == vk::Result::eSuccess;
        if (result != vk::Result::eSuccess && supportsGlobalPriority) {
            qCWarning(KWIN_VULKAN, "High-priority compute queue creation for %s failed: %s; retrying with normal priority",
                      deviceName, vk::to_string(result).c_str());
            queueInfo[computeQueueFamily].pNext = nullptr;
            auto fallback = physicalDevice.createDevice(deviceInfo);
            result = fallback.result;
            logicalDevice = std::move(fallback.value);
            hasHighPriorityComputeQueue = false;
        }
        if (result != vk::Result::eSuccess) {
            qCWarning(KWIN_VULKAN, "vkCreateDevice for %s failed: %s", deviceName, vk::to_string(vk::Result(result)).c_str());
            continue;
        }

        auto ret = std::make_unique<VulkanDevice>(
            physicalDevice,
            std::move(logicalDevice),
            queueProperties | std::ranges::to<std::vector<VkQueueFamilyProperties>>(),
            basicProperties.properties.deviceType,
            computeQueueFamily,
            hasHighPriorityComputeQueue,
            supportsHostQueryReset);
        if (ret->supportedFormats().isEmpty()) {
            continue;
        }
        qCDebug(KWIN_VULKAN, "Found Vulkan device %s for %s (compute queue family %u%s%s)",
                deviceName,
                qPrintable(drm->path()),
                computeQueueFamily,
                ret->hasDedicatedComputeQueue() ? ", dedicated" : "",
                ret->hasHighPriorityComputeQueue() ? ", high priority" : "");
        if (s_forceVulkanGraphicsQueue) {
            qCInfo(KWIN_VULKAN, "KWIN_VULKAN_FORCE_GRAPHICS_QUEUE is set; Vulkan compositor work will use the graphics queue");
        }
        return ret;
    }
    qCDebug(KWIN_VULKAN, "No Vulkan device found for %s", qPrintable(drm->path()));
    return nullptr;
}

void RenderDevice::handleVulkanDeviceLoss()
{
    if (m_inReset) {
        return;
    }
    m_inReset = true;
    // This is done with a queued connection to avoid deleting the Vulkan device
    // before other parts of KWin are able to clean up their Vulkan resources
    QMetaObject::invokeMethod(this, &RenderDevice::createVulkanDevice, Qt::QueuedConnection);
}

void RenderDevice::createVulkanDevice()
{
    if (!*m_vulkan->instance) {
        return;
    }
    m_vulkanDevice = openVulkanDevice(m_vulkan->instance, m_device.get());
    if (m_vulkanDevice) {
        connect(m_vulkanDevice.get(), &VulkanDevice::deviceLost, this, &RenderDevice::handleVulkanDeviceLoss);
    }
    m_inReset = false;
}

bool RenderDevice::isInReset() const
{
    return m_inReset;
}

std::unique_ptr<RenderDevice> RenderDevice::open(const QString &path, int authenticatedFd)
{
    auto drmDevice = DrmDevice::openWithAuthentication(path, authenticatedFd);
    if (!drmDevice) {
        return nullptr;
    }
    auto eglDisplay = EglDisplay::create(eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, drmDevice->gbmDevice(), nullptr), drmDevice.get());
    if (!eglDisplay) {
        return nullptr;
    }
    return std::make_unique<RenderDevice>(std::move(drmDevice), std::move(eglDisplay));
}

bool RenderDevice::isSoftwareDevice() const
{
    return m_display->isSoftwareRenderer() && (!m_vulkanDevice || m_vulkanDevice->isSoftwareRenderer());
}

}
