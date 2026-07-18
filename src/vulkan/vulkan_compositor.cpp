/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "vulkan_compositor.h"

#include "core/colorpipeline.h"
#include "vulkan_device.h"
#include "vulkan_logging.h"
#include "vulkan_render_time_query.h"
#include "vulkan_texture.h"

#include <QFile>
#include <QtGui/rhi/qshader.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <vulkan/vulkan_to_string.hpp>

namespace KWin
{

VulkanCompositorRenderResult::VulkanCompositorRenderResult() = default;
VulkanCompositorRenderResult::VulkanCompositorRenderResult(VulkanCompositorRenderResult &&other) noexcept = default;
VulkanCompositorRenderResult &VulkanCompositorRenderResult::operator=(VulkanCompositorRenderResult &&other) noexcept = default;
VulkanCompositorRenderResult::~VulkanCompositorRenderResult() = default;

namespace
{

struct alignas(16) GpuLayer
{
    std::array<float, 4> rect;
    std::array<float, 4> color;
    std::array<float, 4> sourceRect;
    std::array<float, 4> localRect;
    std::array<float, 4> inverseTransformX;
    std::array<float, 4> inverseTransformY;
    std::array<float, 4> clipRect;
    std::array<float, 4> textureCoordinatesX;
    std::array<float, 4> textureCoordinatesY;
    std::array<float, 4> effects;
    std::array<float, 4> roundedRect;
    std::array<float, 4> cornerRadii;
    std::array<float, 4> colorTransformX;
    std::array<float, 4> colorTransformY;
    std::array<float, 4> colorTransformZ;
    std::array<float, 4> transferParameters;
    std::array<float, 4> colorParameters;
    std::array<float, 4> destinationToLmsX;
    std::array<float, 4> destinationToLmsY;
    std::array<float, 4> destinationToLmsZ;
    std::array<float, 4> lmsToDestinationX;
    std::array<float, 4> lmsToDestinationY;
    std::array<float, 4> lmsToDestinationZ;
    std::array<float, 4> toneMappingParameters;
    std::array<float, 4> yuvTransformX;
    std::array<float, 4> yuvTransformY;
    std::array<float, 4> yuvTransformZ;
    std::array<float, 4> quadPositionX;
    std::array<float, 4> quadPositionY;
    std::array<float, 4> quadTextureX;
    std::array<float, 4> quadTextureY;
    std::array<float, 4> colorFilterMatrixX;
    std::array<float, 4> colorFilterMatrixY;
    std::array<float, 4> colorFilterMatrixZ;
    std::array<float, 4> colorFilterParameters;
    std::array<uint32_t, 4> textureIndices;
    std::array<uint32_t, 4> metadata;
};

struct alignas(16) GpuHotLayer
{
    std::array<float, 4> rect;
    std::array<float, 4> color;
    std::array<float, 4> textureCoordinatesX;
    std::array<float, 4> textureCoordinatesY;
    std::array<float, 4> effects;
    std::array<uint32_t, 4> textureIndices;
    std::array<uint32_t, 4> metadata;
};

struct alignas(16) PushConstants
{
    uint32_t layerCount;
    uint32_t tilesWide;
    uint32_t flags;
    uint32_t dirtyTileCount;
    std::array<float, 4> background;
    uint32_t firstLayer;
    uint32_t lastLayer;
    std::array<uint32_t, 2> padding{};
    std::array<float, 4> outputLutParameters{};
};

static_assert(sizeof(GpuLayer) == 592);
static_assert(sizeof(GpuHotLayer) == 112);
static_assert(sizeof(PushConstants) == 64);

constexpr uint32_t OutputLutEdgeSize = 33;
constexpr size_t OutputLutEntryCount = size_t(OutputLutEdgeSize) * OutputLutEdgeSize * OutputLutEdgeSize;

float fractionalVertex(const QPointF &position)
{
    constexpr double precision = 0.01;
    constexpr double errorCorrection = 1.0 / precision;
    const double correctedX = std::round(position.x() * errorCorrection) / errorCorrection;
    const double correctedY = std::round(position.y() * errorCorrection) / errorCorrection;
    const double errorX = correctedX - std::floor(correctedX);
    const double errorY = correctedY - std::floor(correctedY);
    return errorX + errorY > precision ? 1.0f : 0.0f;
}

bool validateShaderInterface(const QShader &shader, const QString &path)
{
    const QShaderDescription description = shader.description();
    std::array<bool, 9> bindings{};
    const auto recordBinding = [&path, &bindings](int descriptorSet, int binding, std::initializer_list<int> allowed) {
        if (descriptorSet != 0 || std::ranges::find(allowed, binding) == allowed.end()
            || binding < 0 || size_t(binding) >= bindings.size() || bindings[binding]) {
            qCWarning(KWIN_VULKAN) << "Vulkan compositor shader has an unexpected descriptor binding" << path << descriptorSet << binding;
            return false;
        }
        bindings[binding] = true;
        return true;
    };

    for (const QShaderDescription::StorageBlock &block : description.storageBlocks()) {
        if (!recordBinding(block.descriptorSet, block.binding, {0, 1, 4, 5, 6, 7, 8})) {
            return false;
        }
    }
    for (const QShaderDescription::InOutVariable &image : description.storageImages()) {
        if (!recordBinding(image.descriptorSet, image.binding, {2})) {
            return false;
        }
    }
    for (const QShaderDescription::InOutVariable &sampler : description.combinedImageSamplers()) {
        if (!recordBinding(sampler.descriptorSet, sampler.binding, {3})
            || sampler.arrayDims != QList<int>{int(VulkanCompositor::MaximumTextureCount)}) {
            qCWarning(KWIN_VULKAN) << "Vulkan compositor shader has an unexpected sampler array" << path;
            return false;
        }
    }
    if (!description.uniformBlocks().isEmpty() || !description.separateImages().isEmpty() || !description.separateSamplers().isEmpty()) {
        qCWarning(KWIN_VULKAN) << "Vulkan compositor shader uses unsupported descriptor types" << path;
        return false;
    }

    const bool simpleCompositeShader = path.contains(QStringLiteral("tile_composite_simple"));
    const bool colorCompositeShader = path.contains(QStringLiteral("tile_composite_color"));
    const bool compositeShader = path.contains(QStringLiteral("tile_composite"));
    const bool prefixPropagateShader = path.contains(QStringLiteral("tile_prefix_propagate"));
    const bool prefixFinalizeShader = path.contains(QStringLiteral("tile_prefix_finalize"));
    const bool bruteForceMaskShader = path.contains(QStringLiteral("tile_mask_bruteforce"));
    const std::array expectedBindings = simpleCompositeShader
        ? std::array{false, true, true, true, true, false, true, false, false}
        : colorCompositeShader
        ? std::array{true, true, true, true, true, false, false, false, false}
        : compositeShader
        ? std::array{true, true, true, true, true, true, false, false, false}
        : prefixPropagateShader
        ? std::array{false, false, false, false, false, false, false, true, true}
        : prefixFinalizeShader
        ? std::array{false, true, false, false, false, false, false, true, true}
        : bruteForceMaskShader
        ? std::array{false, true, false, false, true, false, true, false, false}
        : std::array{false, false, false, false, false, false, true, true, false};
    if (bindings != expectedBindings) {
        qCWarning(KWIN_VULKAN) << "Vulkan compositor shader descriptor interface is incomplete" << path;
        return false;
    }
    const QList<QShaderDescription::PushConstantBlock> pushConstants = description.pushConstantBlocks();
    if (pushConstants.size() != 1 || pushConstants.front().size != sizeof(PushConstants)) {
        qCWarning(KWIN_VULKAN) << "Vulkan compositor shader has an unexpected push constant layout" << path;
        return false;
    }
    return true;
}

QByteArray loadSpirv(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qCWarning(KWIN_VULKAN) << "Failed to open Vulkan compositor shader" << path;
        return {};
    }
    const QShader shader = QShader::fromSerialized(file.readAll());
    if (!shader.isValid()) {
        qCWarning(KWIN_VULKAN) << "Invalid Vulkan compositor shader" << path;
        return {};
    }
    if (!validateShaderInterface(shader, path)) {
        return {};
    }
    const QList<QShaderKey> availableShaders = shader.availableShaders();
    const auto keyIt = std::ranges::find_if(availableShaders, [](const QShaderKey &key) {
        return key.source() == QShader::SpirvShader && key.sourceVariant() == QShader::StandardShader;
    });
    if (keyIt == availableShaders.end()) {
        qCWarning(KWIN_VULKAN) << "Vulkan compositor shader has no SPIR-V variant" << path;
        return {};
    }
    return shader.shader(*keyIt).shader();
}

std::optional<vk::raii::ShaderModule> createShaderModule(const vk::raii::Device &device, const QString &path)
{
    const QByteArray code = loadSpirv(path);
    if (code.isEmpty() || code.size() % sizeof(uint32_t) != 0) {
        return std::nullopt;
    }
    auto [result, module] = device.createShaderModule(vk::ShaderModuleCreateInfo{
        vk::ShaderModuleCreateFlags{},
        size_t(code.size()),
        reinterpret_cast<const uint32_t *>(code.constData()),
    });
    if (result != vk::Result::eSuccess) {
        qCWarning(KWIN_VULKAN) << "Failed to create Vulkan compositor shader module:" << vk::to_string(result);
        return std::nullopt;
    }
    return std::move(module);
}

std::array<float, 4> premultiplied(const QColor &color)
{
    const QColor rgb = color.toRgb();
    const float alpha = rgb.alphaF();
    return {
        float(rgb.redF() * alpha),
        float(rgb.greenF() * alpha),
        float(rgb.blueF() * alpha),
        alpha,
    };
}

std::array<float, 4> premultiplied(const QColor &color, qreal opacity)
{
    const QColor rgb = color.toRgb();
    const float alpha = float(rgb.alphaF() * std::clamp(opacity, 0.0, 1.0));
    return {
        float(rgb.redF() * alpha),
        float(rgb.greenF() * alpha),
        float(rgb.blueF() * alpha),
        alpha,
    };
}

std::array<float, 2> transferParameters(const TransferFunction &transferFunction)
{
    if (transferFunction.type == TransferFunction::BT1886) {
        return {float(transferFunction.bt1886B()), float(transferFunction.bt1886A())};
    }
    return {float(transferFunction.minLuminance), float(transferFunction.maxLuminance - transferFunction.minLuminance)};
}

std::array<float, 4> matrixRow(const QMatrix4x4 &matrix, int row)
{
    return {matrix(row, 0), matrix(row, 1), matrix(row, 2), matrix(row, 3)};
}

} // namespace

VulkanCompositor::FrameResources::FrameResources()
    : descriptorPool(nullptr)
    , layerBuffer(nullptr)
    , layerMemory(nullptr)
    , hotLayerBuffer(nullptr)
    , hotLayerMemory(nullptr)
    , tileBuffer(nullptr)
    , tileMemory(nullptr)
    , prefixBuffer(nullptr)
    , prefixMemory(nullptr)
    , carryBuffer(nullptr)
    , carryMemory(nullptr)
    , dirtyTileBuffer(nullptr)
    , dirtyTileMemory(nullptr)
    , outputLutBuffer(nullptr)
    , outputLutMemory(nullptr)
{
}

VulkanCompositor::FrameResources::~FrameResources() = default;

void VulkanCompositor::FrameResources::unmapHostMemory()
{
    if (layerData) {
        layerMemory.unmapMemory();
        layerData = nullptr;
    }
    if (hotLayerData) {
        hotLayerMemory.unmapMemory();
        hotLayerData = nullptr;
    }
    if (dirtyTileData) {
        dirtyTileMemory.unmapMemory();
        dirtyTileData = nullptr;
    }
    if (outputLutData) {
        outputLutMemory.unmapMemory();
        outputLutData = nullptr;
    }
}

VulkanCompositor::VulkanCompositor(VulkanDevice *device)
    : m_device(device)
    , m_descriptorSetLayout(nullptr)
    , m_pipelineLayout(nullptr)
    , m_preprocessPipeline(nullptr)
    , m_bruteForceMaskPipeline(nullptr)
    , m_prefixPropagatePipeline(nullptr)
    , m_prefixFinalizePipeline(nullptr)
    , m_compositePipeline(nullptr)
    , m_colorCompositePipeline(nullptr)
    , m_simpleCompositePipeline(nullptr)
    , m_shallowCompositePipeline(nullptr)
    , m_sampler(nullptr)
{
    connect(device, &VulkanDevice::deviceLost, this, [this]() {
        releaseResources();
        m_device = nullptr;
    });
}

VulkanCompositor::~VulkanCompositor()
{
    if (m_device) {
        m_device->waitIdle();
    }
    releaseResources();
}

std::unique_ptr<VulkanCompositor> VulkanCompositor::create(VulkanDevice *device)
{
    if (!device) {
        return nullptr;
    }
    auto compositor = std::unique_ptr<VulkanCompositor>(new VulkanCompositor(device));
    if (!compositor->initialize()) {
        return nullptr;
    }
    return compositor;
}

bool VulkanCompositor::initialize()
{
    const auto &device = m_device->logicalDevice();
    std::vector<vk::DescriptorSetLayoutBinding> bindings{
        {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        {2, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
        {3, vk::DescriptorType::eCombinedImageSampler, MaximumTextureCount, vk::ShaderStageFlagBits::eCompute},
        {4, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        {5, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        {6, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        {7, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        {8, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
    };
    auto [layoutResult, descriptorSetLayout] = device.createDescriptorSetLayout(vk::DescriptorSetLayoutCreateInfo{
        vk::DescriptorSetLayoutCreateFlags{},
        bindings,
    });
    if (layoutResult != vk::Result::eSuccess) {
        return false;
    }
    m_descriptorSetLayout = std::move(descriptorSetLayout);

    const vk::PushConstantRange pushRange{vk::ShaderStageFlagBits::eCompute, 0, sizeof(PushConstants)};
    auto [pipelineLayoutResult, pipelineLayout] = device.createPipelineLayout(vk::PipelineLayoutCreateInfo{
        vk::PipelineLayoutCreateFlags{},
        *m_descriptorSetLayout,
        pushRange,
    });
    if (pipelineLayoutResult != vk::Result::eSuccess) {
        return false;
    }
    m_pipelineLayout = std::move(pipelineLayout);

    auto [samplerResult, sampler] = device.createSampler(vk::SamplerCreateInfo{
        vk::SamplerCreateFlags{},
        vk::Filter::eLinear,
        vk::Filter::eLinear,
        vk::SamplerMipmapMode::eNearest,
        vk::SamplerAddressMode::eClampToEdge,
        vk::SamplerAddressMode::eClampToEdge,
        vk::SamplerAddressMode::eClampToEdge,
        0.0f,
        false,
        1.0f,
        false,
        vk::CompareOp::eNever,
        0.0f,
        0.0f,
        vk::BorderColor::eFloatTransparentBlack,
        false,
    });
    if (samplerResult != vk::Result::eSuccess) {
        return false;
    }
    m_sampler = std::move(sampler);

    QImage transparent(1, 1, QImage::Format_RGBA8888_Premultiplied);
    transparent.fill(Qt::transparent);
    m_fallbackTexture = VulkanTexture::upload(m_device,
                                              transparent,
                                              vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
                                              VulkanQueueRole::Compute);
    if (!m_fallbackTexture) {
        return false;
    }
    if (!m_fallbackTexture->imageView()) {
        return false;
    }

    return createPipelines();
}

bool VulkanCompositor::createPipelines()
{
    const auto preprocessModule = createShaderModule(m_device->logicalDevice(), QStringLiteral(":/vulkan/tile_preprocess.comp.qsb"));
    const auto bruteForceMaskModule = createShaderModule(m_device->logicalDevice(), QStringLiteral(":/vulkan/tile_mask_bruteforce.comp.qsb"));
    const auto prefixPropagateModule = createShaderModule(m_device->logicalDevice(), QStringLiteral(":/vulkan/tile_prefix_propagate.comp.qsb"));
    const auto prefixFinalizeModule = createShaderModule(m_device->logicalDevice(), QStringLiteral(":/vulkan/tile_prefix_finalize.comp.qsb"));
    const auto compositeModule = createShaderModule(m_device->logicalDevice(), QStringLiteral(":/vulkan/tile_composite.comp.qsb"));
    const auto colorCompositeModule = createShaderModule(m_device->logicalDevice(), QStringLiteral(":/vulkan/tile_composite_color.comp.qsb"));
    const auto simpleCompositeModule = createShaderModule(m_device->logicalDevice(), QStringLiteral(":/vulkan/tile_composite_simple.comp.qsb"));
    const auto shallowCompositeModule = createShaderModule(m_device->logicalDevice(), QStringLiteral(":/vulkan/tile_composite_simple_2x2.comp.qsb"));
    if (!preprocessModule || !bruteForceMaskModule || !prefixPropagateModule || !prefixFinalizeModule
        || !compositeModule || !colorCompositeModule || !simpleCompositeModule || !shallowCompositeModule) {
        return false;
    }

    const auto createPipeline = [this](const vk::raii::ShaderModule &module) -> std::optional<vk::raii::Pipeline> {
        const vk::PipelineShaderStageCreateInfo shaderStage{
            vk::PipelineShaderStageCreateFlags{},
            vk::ShaderStageFlagBits::eCompute,
            *module,
            "main",
        };
        auto [result, pipeline] = m_device->logicalDevice().createComputePipeline(nullptr, vk::ComputePipelineCreateInfo{
                                                                                               vk::PipelineCreateFlags{},
                                                                                               shaderStage,
                                                                                               *m_pipelineLayout,
                                                                                           });
        if (result != vk::Result::eSuccess) {
            qCWarning(KWIN_VULKAN) << "Failed to create Vulkan compositor pipeline:" << vk::to_string(result);
            return std::nullopt;
        }
        return std::move(pipeline);
    };

    auto preprocessPipeline = createPipeline(*preprocessModule);
    auto bruteForceMaskPipeline = createPipeline(*bruteForceMaskModule);
    auto prefixPropagatePipeline = createPipeline(*prefixPropagateModule);
    auto prefixFinalizePipeline = createPipeline(*prefixFinalizeModule);
    auto compositePipeline = createPipeline(*compositeModule);
    auto colorCompositePipeline = createPipeline(*colorCompositeModule);
    auto simpleCompositePipeline = createPipeline(*simpleCompositeModule);
    auto shallowCompositePipeline = createPipeline(*shallowCompositeModule);
    if (!preprocessPipeline || !bruteForceMaskPipeline || !prefixPropagatePipeline || !prefixFinalizePipeline
        || !compositePipeline || !colorCompositePipeline || !simpleCompositePipeline || !shallowCompositePipeline) {
        return false;
    }
    m_preprocessPipeline = std::move(*preprocessPipeline);
    m_bruteForceMaskPipeline = std::move(*bruteForceMaskPipeline);
    m_prefixPropagatePipeline = std::move(*prefixPropagatePipeline);
    m_prefixFinalizePipeline = std::move(*prefixFinalizePipeline);
    m_compositePipeline = std::move(*compositePipeline);
    m_colorCompositePipeline = std::move(*colorCompositePipeline);
    m_simpleCompositePipeline = std::move(*simpleCompositePipeline);
    m_shallowCompositePipeline = std::move(*shallowCompositePipeline);
    return true;
}

bool VulkanCompositor::acquireFrameResources()
{
    for (uint32_t offset = 0; offset < FrameResourceCount; ++offset) {
        const uint32_t index = (m_nextFrame + offset) % FrameResourceCount;
        FrameResources &frame = m_frames[index];
        if (!frame.completionFence.isValid() || frame.completionFence.isReadable()) {
            frame.completionFence = FileDescriptor{};
            m_currentFrame = &frame;
            m_nextFrame = (index + 1) % FrameResourceCount;
            return true;
        }
    }

    // Three frames in flight is already beyond the normal output swapchain
    // depth. Only block when a caller actually exhausts the ring.
    m_device->waitComputeIdle();
    for (FrameResources &frame : m_frames) {
        frame.completionFence = FileDescriptor{};
    }
    m_currentFrame = &m_frames[m_nextFrame];
    m_nextFrame = (m_nextFrame + 1) % FrameResourceCount;
    return true;
}

bool VulkanCompositor::ensureTarget(const QSize &size)
{
    if (m_texture && size == m_texture->size()) {
        return true;
    }
    if (size.isEmpty()) {
        return false;
    }

    m_device->waitComputeIdle();
    for (FrameResources &frame : m_frames) {
        frame.completionFence = FileDescriptor{};
    }
    m_texture.reset();

    m_texture = VulkanTexture::allocate(m_device,
                                        vk::Format::eR8G8B8A8Unorm,
                                        size,
                                        vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
                                        VulkanQueueRole::Compute);
    return bool(m_texture);
}

bool VulkanCompositor::ensureResources(const QSize &size, size_t layerCount)
{
    const bool allFramesValid = std::ranges::all_of(m_frames, [](const FrameResources &frame) {
        return *frame.layerBuffer && *frame.hotLayerBuffer
            && *frame.tileBuffer && *frame.prefixBuffer && *frame.carryBuffer
            && *frame.dirtyTileBuffer && *frame.outputLutBuffer;
    });
    if (size == m_size && layerCount <= m_layerCapacity && allFramesValid) {
        return true;
    }
    if (size.isEmpty() || layerCount > std::numeric_limits<uint32_t>::max()) {
        return false;
    }

    const size_t requiredCapacity = std::max<size_t>(layerCount, 1);
    const size_t layerCapacity = std::bit_ceil(requiredCapacity);
    m_device->waitComputeIdle();
    for (FrameResources &frame : m_frames) {
        frame.unmapHostMemory();
        frame.layerBuffer.clear();
        frame.layerMemory.clear();
        frame.hotLayerBuffer.clear();
        frame.hotLayerMemory.clear();
        frame.tileBuffer.clear();
        frame.tileMemory.clear();
        frame.prefixBuffer.clear();
        frame.prefixMemory.clear();
        frame.carryBuffer.clear();
        frame.carryMemory.clear();
        frame.dirtyTileBuffer.clear();
        frame.dirtyTileMemory.clear();
        frame.outputLutBuffer.clear();
        frame.outputLutMemory.clear();
        frame.outputColorPipeline.reset();
        frame.completionFence = FileDescriptor{};
    }
    // Do not let a partial allocation failure make the next call mistake the
    // incomplete frame ring for resources matching the old dimensions.
    m_size = {};
    m_layerCapacity = 0;

    const uint32_t tilesWide = (size.width() + TileSize - 1) / TileSize;
    const uint32_t tilesHigh = (size.height() + TileSize - 1) / TileSize;
    const uint32_t binsWide = (tilesWide + TileSize - 1) / TileSize;
    const uint32_t binsHigh = (tilesHigh + TileSize - 1) / TileSize;
    const size_t maskWordCapacity = (layerCapacity + 31) / 32;
    const size_t summaryWordCapacity = (maskWordCapacity + 31) / 32;
    if (tilesWide > 0x10000u || tilesHigh > 0x10000u) {
        return false;
    }
    const vk::BufferCreateInfo layerBufferInfo{
        vk::BufferCreateFlags{},
        vk::DeviceSize(sizeof(GpuLayer)) * layerCapacity,
        vk::BufferUsageFlagBits::eStorageBuffer,
    };
    const vk::BufferCreateInfo hotLayerBufferInfo{
        vk::BufferCreateFlags{},
        vk::DeviceSize(sizeof(GpuHotLayer)) * layerCapacity,
        vk::BufferUsageFlagBits::eStorageBuffer,
    };
    const vk::BufferCreateInfo tileBufferInfo{
        vk::BufferCreateFlags{},
        vk::DeviceSize(tilesWide) * tilesHigh * (maskWordCapacity + summaryWordCapacity) * sizeof(uint32_t),
        vk::BufferUsageFlagBits::eStorageBuffer,
    };
    const vk::BufferCreateInfo prefixBufferInfo{
        vk::BufferCreateFlags{},
        vk::DeviceSize(tilesWide) * tilesHigh * maskWordCapacity * 2 * sizeof(uint32_t),
        vk::BufferUsageFlagBits::eStorageBuffer,
    };
    const vk::BufferCreateInfo carryBufferInfo{
        vk::BufferCreateFlags{},
        vk::DeviceSize(size_t(binsWide) * tilesHigh + size_t(tilesWide) * binsHigh) * maskWordCapacity * 2 * sizeof(uint32_t),
        vk::BufferUsageFlagBits::eStorageBuffer,
    };
    const vk::BufferCreateInfo dirtyTileBufferInfo{
        vk::BufferCreateFlags{},
        vk::DeviceSize(tilesWide) * tilesHigh * sizeof(uint32_t),
        vk::BufferUsageFlagBits::eStorageBuffer,
    };
    const vk::BufferCreateInfo outputLutBufferInfo{
        vk::BufferCreateFlags{},
        vk::DeviceSize(OutputLutEntryCount) * sizeof(std::array<float, 4>),
        vk::BufferUsageFlagBits::eStorageBuffer,
    };
    for (FrameResources &frame : m_frames) {
        frame.layerMemory = m_device->allocateMemory(layerBufferInfo,
                                                     vk::MemoryPropertyFlagBits::eDeviceLocal
                                                         | vk::MemoryPropertyFlagBits::eHostVisible
                                                         | vk::MemoryPropertyFlagBits::eHostCoherent);
        if (!*frame.layerMemory) {
            frame.layerMemory = m_device->allocateMemory(layerBufferInfo,
                                                         vk::MemoryPropertyFlagBits::eHostVisible
                                                             | vk::MemoryPropertyFlagBits::eHostCoherent);
        }
        if (!*frame.layerMemory) {
            return false;
        }
        auto [layerBufferResult, layerBuffer] = m_device->logicalDevice().createBuffer(layerBufferInfo);
        if (layerBufferResult != vk::Result::eSuccess) {
            return false;
        }
        frame.layerBuffer = std::move(layerBuffer);
        if (frame.layerBuffer.bindMemory(frame.layerMemory, 0) != vk::Result::eSuccess) {
            return false;
        }
        auto [layerMapResult, layerData] = frame.layerMemory.mapMemory(0, VK_WHOLE_SIZE);
        if (layerMapResult != vk::Result::eSuccess) {
            return false;
        }
        frame.layerData = layerData;

        frame.hotLayerMemory = m_device->allocateMemory(hotLayerBufferInfo,
                                                        vk::MemoryPropertyFlagBits::eDeviceLocal
                                                            | vk::MemoryPropertyFlagBits::eHostVisible
                                                            | vk::MemoryPropertyFlagBits::eHostCoherent);
        if (!*frame.hotLayerMemory) {
            frame.hotLayerMemory = m_device->allocateMemory(hotLayerBufferInfo,
                                                            vk::MemoryPropertyFlagBits::eHostVisible
                                                                | vk::MemoryPropertyFlagBits::eHostCoherent);
        }
        if (!*frame.hotLayerMemory) {
            return false;
        }
        auto [hotLayerBufferResult, hotLayerBuffer] = m_device->logicalDevice().createBuffer(hotLayerBufferInfo);
        if (hotLayerBufferResult != vk::Result::eSuccess) {
            return false;
        }
        frame.hotLayerBuffer = std::move(hotLayerBuffer);
        if (frame.hotLayerBuffer.bindMemory(frame.hotLayerMemory, 0) != vk::Result::eSuccess) {
            return false;
        }
        auto [hotLayerMapResult, hotLayerData] = frame.hotLayerMemory.mapMemory(0, VK_WHOLE_SIZE);
        if (hotLayerMapResult != vk::Result::eSuccess) {
            return false;
        }
        frame.hotLayerData = hotLayerData;

        frame.tileMemory = m_device->allocateMemory(tileBufferInfo, vk::MemoryPropertyFlagBits::eDeviceLocal);
        if (!*frame.tileMemory) {
            return false;
        }
        auto [bufferResult, buffer] = m_device->logicalDevice().createBuffer(tileBufferInfo);
        if (bufferResult != vk::Result::eSuccess) {
            return false;
        }
        frame.tileBuffer = std::move(buffer);
        if (frame.tileBuffer.bindMemory(frame.tileMemory, 0) != vk::Result::eSuccess) {
            return false;
        }

        frame.prefixMemory = m_device->allocateMemory(prefixBufferInfo, vk::MemoryPropertyFlagBits::eDeviceLocal);
        if (!*frame.prefixMemory) {
            return false;
        }
        auto [prefixBufferResult, prefixBuffer] = m_device->logicalDevice().createBuffer(prefixBufferInfo);
        if (prefixBufferResult != vk::Result::eSuccess) {
            return false;
        }
        frame.prefixBuffer = std::move(prefixBuffer);
        if (frame.prefixBuffer.bindMemory(frame.prefixMemory, 0) != vk::Result::eSuccess) {
            return false;
        }

        frame.carryMemory = m_device->allocateMemory(carryBufferInfo, vk::MemoryPropertyFlagBits::eDeviceLocal);
        if (!*frame.carryMemory) {
            return false;
        }
        auto [carryBufferResult, carryBuffer] = m_device->logicalDevice().createBuffer(carryBufferInfo);
        if (carryBufferResult != vk::Result::eSuccess) {
            return false;
        }
        frame.carryBuffer = std::move(carryBuffer);
        if (frame.carryBuffer.bindMemory(frame.carryMemory, 0) != vk::Result::eSuccess) {
            return false;
        }

        frame.dirtyTileMemory = m_device->allocateMemory(dirtyTileBufferInfo, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        if (!*frame.dirtyTileMemory) {
            return false;
        }
        auto [dirtyBufferResult, dirtyBuffer] = m_device->logicalDevice().createBuffer(dirtyTileBufferInfo);
        if (dirtyBufferResult != vk::Result::eSuccess) {
            return false;
        }
        frame.dirtyTileBuffer = std::move(dirtyBuffer);
        if (frame.dirtyTileBuffer.bindMemory(frame.dirtyTileMemory, 0) != vk::Result::eSuccess) {
            return false;
        }
        auto [dirtyTileMapResult, dirtyTileData] = frame.dirtyTileMemory.mapMemory(0, VK_WHOLE_SIZE);
        if (dirtyTileMapResult != vk::Result::eSuccess) {
            return false;
        }
        frame.dirtyTileData = dirtyTileData;

        frame.outputLutMemory = m_device->allocateMemory(outputLutBufferInfo, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        if (!*frame.outputLutMemory) {
            return false;
        }
        auto [outputLutBufferResult, outputLutBuffer] = m_device->logicalDevice().createBuffer(outputLutBufferInfo);
        if (outputLutBufferResult != vk::Result::eSuccess) {
            return false;
        }
        frame.outputLutBuffer = std::move(outputLutBuffer);
        if (frame.outputLutBuffer.bindMemory(frame.outputLutMemory, 0) != vk::Result::eSuccess) {
            return false;
        }
        auto [outputLutMapResult, outputLutData] = frame.outputLutMemory.mapMemory(0, VK_WHOLE_SIZE);
        if (outputLutMapResult != vk::Result::eSuccess) {
            return false;
        }
        frame.outputLutData = outputLutData;
    }
    m_size = size;
    m_layerCapacity = layerCapacity;
    return true;
}

bool VulkanCompositor::ensureDescriptorSets(uint32_t batchCount)
{
    if (!m_currentFrame || batchCount == 0) {
        return false;
    }
    if (m_currentFrame->descriptorCapacity >= batchCount
        && m_currentFrame->descriptorSets.size() >= batchCount) {
        return true;
    }

    const uint32_t capacity = std::bit_ceil(batchCount);
    m_currentFrame->descriptorSets.clear();
    m_currentFrame->descriptorPool.clear();
    const std::array poolSizes{
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 7 * capacity},
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, capacity},
        vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler, MaximumTextureCount * capacity},
    };
    auto [poolResult, pool] = m_device->logicalDevice().createDescriptorPool(vk::DescriptorPoolCreateInfo{
        vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
        capacity,
        poolSizes,
    });
    if (poolResult != vk::Result::eSuccess) {
        return false;
    }
    m_currentFrame->descriptorPool = std::move(pool);

    std::vector<vk::DescriptorSetLayout> layouts(capacity, *m_descriptorSetLayout);
    auto [setsResult, sets] = m_device->logicalDevice().allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
        *m_currentFrame->descriptorPool,
        layouts,
    });
    if (setsResult != vk::Result::eSuccess || sets.size() != capacity) {
        return false;
    }
    m_currentFrame->descriptorSets = std::move(sets);
    m_currentFrame->descriptorCapacity = capacity;
    return true;
}

bool VulkanCompositor::updateBufferDescriptors(uint32_t batchCount)
{
    if (!m_currentFrame || batchCount > m_currentFrame->descriptorSets.size()) {
        return false;
    }
    const vk::DescriptorBufferInfo layerInfo{*m_currentFrame->layerBuffer, 0, vk::WholeSize};
    const vk::DescriptorBufferInfo hotLayerInfo{*m_currentFrame->hotLayerBuffer, 0, vk::WholeSize};
    const vk::DescriptorBufferInfo tileInfo{*m_currentFrame->tileBuffer, 0, vk::WholeSize};
    const vk::DescriptorBufferInfo prefixInfo{*m_currentFrame->prefixBuffer, 0, vk::WholeSize};
    const vk::DescriptorBufferInfo carryInfo{*m_currentFrame->carryBuffer, 0, vk::WholeSize};
    const vk::DescriptorBufferInfo dirtyTileInfo{*m_currentFrame->dirtyTileBuffer, 0, vk::WholeSize};
    const vk::DescriptorBufferInfo outputLutInfo{*m_currentFrame->outputLutBuffer, 0, vk::WholeSize};
    for (uint32_t i = 0; i < batchCount; ++i) {
        const std::array writes{
            vk::WriteDescriptorSet{*m_currentFrame->descriptorSets[i], 0, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &layerInfo},
            vk::WriteDescriptorSet{*m_currentFrame->descriptorSets[i], 1, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &tileInfo},
            vk::WriteDescriptorSet{*m_currentFrame->descriptorSets[i], 4, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &dirtyTileInfo},
            vk::WriteDescriptorSet{*m_currentFrame->descriptorSets[i], 5, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &outputLutInfo},
            vk::WriteDescriptorSet{*m_currentFrame->descriptorSets[i], 6, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &hotLayerInfo},
            vk::WriteDescriptorSet{*m_currentFrame->descriptorSets[i], 7, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &prefixInfo},
            vk::WriteDescriptorSet{*m_currentFrame->descriptorSets[i], 8, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &carryInfo},
        };
        m_device->logicalDevice().updateDescriptorSets(writes, {});
    }
    return true;
}

bool VulkanCompositor::updateTargetDescriptor(VulkanTexture *target, uint32_t batchCount)
{
    const auto supportedTargetFormat = [](vk::Format format) {
        switch (format) {
        case vk::Format::eR8G8B8A8Unorm:
        case vk::Format::eB8G8R8A8Unorm:
        case vk::Format::eA2R10G10B10UnormPack32:
        case vk::Format::eA2B10G10R10UnormPack32:
        case vk::Format::eR16G16B16A16Unorm:
        case vk::Format::eR16G16B16A16Sfloat:
            return true;
        default:
            return false;
        }
    };
    if (!target || target->size() != m_size || !supportedTargetFormat(target->format())) {
        qCWarning(KWIN_VULKAN) << "The Vulkan compositor target has an unsupported size or format";
        return false;
    }
    if (m_device->hasDedicatedComputeQueue()
        && target->queueRole() != VulkanQueueRole::Compute
        && target->queueRole() != VulkanQueueRole::Concurrent) {
        qCWarning(KWIN_VULKAN) << "A target without compute-queue ownership was passed to the Vulkan compositor";
        return false;
    }
    if (!m_currentFrame || batchCount > m_currentFrame->descriptorSets.size()) {
        return false;
    }
    const vk::ImageView imageView = target->imageView();
    if (!imageView) {
        return false;
    }
    const vk::DescriptorImageInfo imageInfo{nullptr, imageView, vk::ImageLayout::eGeneral};
    for (uint32_t i = 0; i < batchCount; ++i) {
        m_device->logicalDevice().updateDescriptorSets(vk::WriteDescriptorSet{
                                                           *m_currentFrame->descriptorSets[i],
                                                           2,
                                                           0,
                                                           1,
                                                           vk::DescriptorType::eStorageImage,
                                                           &imageInfo,
                                                       },
                                                       {});
    }
    return true;
}

bool VulkanCompositor::updateTextureDescriptors(std::span<const TextureBatch> batches,
                                                std::vector<vk::ImageMemoryBarrier2> &acquireBarriers,
                                                std::vector<vk::ImageMemoryBarrier2> &releaseBarriers)
{
    if (!m_currentFrame || batches.size() > m_currentFrame->descriptorSets.size()) {
        return false;
    }
    std::vector<VulkanTexture *> textures;
    for (const TextureBatch &batch : batches) {
        if (batch.textures.size() > MaximumTextureCount) {
            return false;
        }
        for (VulkanTexture *texture : batch.textures) {
            if (!std::ranges::contains(textures, texture)) {
                textures.push_back(texture);
            }
        }
    }
    std::vector<vk::ImageView> imageViews;
    imageViews.reserve(textures.size());
    acquireBarriers.clear();
    acquireBarriers.reserve(textures.size());
    releaseBarriers.clear();
    releaseBarriers.reserve(textures.size());
    for (VulkanTexture *texture : textures) {
        if (!texture || texture->size().isEmpty()) {
            return false;
        }
        if (m_device->hasDedicatedComputeQueue()
            && texture->queueRole() != VulkanQueueRole::Compute
            && texture->queueRole() != VulkanQueueRole::Concurrent) {
            qCWarning(KWIN_VULKAN) << "A texture without compute-queue ownership was passed to the Vulkan compositor";
            return false;
        }
        const vk::ImageView view = texture->imageView();
        if (!view) {
            return false;
        }
        imageViews.push_back(view);
        const bool external = texture->isExternal();
        acquireBarriers.emplace_back(external ? vk::PipelineStageFlagBits2::eNone : vk::PipelineStageFlagBits2::eAllCommands,
                                     external ? vk::AccessFlags2{} : vk::AccessFlags2(vk::AccessFlagBits2::eMemoryWrite),
                                     vk::PipelineStageFlagBits2::eComputeShader,
                                     vk::AccessFlagBits2::eShaderRead,
                                     vk::ImageLayout::eGeneral,
                                     vk::ImageLayout::eGeneral,
                                     external ? vk::QueueFamilyExternal : vk::QueueFamilyIgnored,
                                     external ? m_device->computeQueueFamily() : vk::QueueFamilyIgnored,
                                     texture->handle(),
                                     vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
        if (external) {
            releaseBarriers.emplace_back(vk::PipelineStageFlagBits2::eComputeShader,
                                         vk::AccessFlagBits2::eShaderRead,
                                         vk::PipelineStageFlagBits2::eNone,
                                         vk::AccessFlags2{},
                                         vk::ImageLayout::eGeneral,
                                         vk::ImageLayout::eGeneral,
                                         m_device->computeQueueFamily(),
                                         vk::QueueFamilyExternal,
                                         texture->handle(),
                                         vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
        }
    }

    for (size_t batchIndex = 0; batchIndex < batches.size(); ++batchIndex) {
        std::array<vk::DescriptorImageInfo, MaximumTextureCount> imageInfos;
        const TextureBatch &batch = batches[batchIndex];
        for (uint32_t i = 0; i < MaximumTextureCount; ++i) {
            vk::ImageView view = m_fallbackTexture->imageView();
            if (i < batch.textures.size()) {
                const auto textureIt = std::ranges::find(textures, batch.textures[i]);
                if (textureIt == textures.end()) {
                    return false;
                }
                view = imageViews[std::distance(textures.begin(), textureIt)];
            }
            imageInfos[i] = vk::DescriptorImageInfo{*m_sampler, view, vk::ImageLayout::eGeneral};
        }
        m_device->logicalDevice().updateDescriptorSets(vk::WriteDescriptorSet{
                                                           *m_currentFrame->descriptorSets[batchIndex],
                                                           3,
                                                           0,
                                                           MaximumTextureCount,
                                                           vk::DescriptorType::eCombinedImageSampler,
                                                           imageInfos.data(),
                                                       },
                                                       {});
    }
    return true;
}

bool VulkanCompositor::updateOutputLut(const ColorPipeline &pipeline)
{
    if (!m_currentFrame || !*m_currentFrame->outputLutMemory) {
        return false;
    }
    if (m_currentFrame->outputColorPipeline
        && *m_currentFrame->outputColorPipeline == pipeline) {
        return true;
    }

    auto entries = std::span(static_cast<std::array<float, 4> *>(m_currentFrame->outputLutData), OutputLutEntryCount);
    const float minimum = pipeline.inputRange.min;
    const float range = pipeline.inputRange.max - pipeline.inputRange.min;
    for (uint32_t blue = 0; blue < OutputLutEdgeSize; ++blue) {
        for (uint32_t green = 0; green < OutputLutEdgeSize; ++green) {
            for (uint32_t red = 0; red < OutputLutEdgeSize; ++red) {
                const QVector3D input(minimum + range * red / float(OutputLutEdgeSize - 1),
                                      minimum + range * green / float(OutputLutEdgeSize - 1),
                                      minimum + range * blue / float(OutputLutEdgeSize - 1));
                const QVector3D output = pipeline.evaluate(input);
                const size_t index = size_t(blue) * OutputLutEdgeSize * OutputLutEdgeSize
                    + size_t(green) * OutputLutEdgeSize + red;
                entries[index] = {output.x(), output.y(), output.z(), 1.0f};
            }
        }
    }
    m_currentFrame->outputColorPipeline = std::make_unique<ColorPipeline>(pipeline);
    return true;
}

std::optional<VulkanCompositorRenderResult> VulkanCompositor::render(const QSize &size,
                                                                     std::span<const VulkanSolidLayer> layers,
                                                                     const QColor &background,
                                                                     const Region &damage)
{
    std::vector<VulkanCompositorLayer> compositorLayers;
    compositorLayers.reserve(layers.size());
    for (const VulkanSolidLayer &layer : layers) {
        compositorLayers.push_back(VulkanCompositorLayer{
            .rect = layer.rect,
            .color = layer.color,
        });
    }
    return render(size, compositorLayers, background, damage);
}

std::optional<VulkanCompositorRenderResult> VulkanCompositor::render(const QSize &size,
                                                                     std::span<const VulkanCompositorLayer> layers,
                                                                     const QColor &background,
                                                                     const Region &damage,
                                                                     const std::shared_ptr<ColorDescription> &targetColorDescription,
                                                                     const ColorPipeline *outputColorPipeline,
                                                                     FileDescriptor &&acquireFence,
                                                                     VulkanUploadManager *uploadManager)
{
    if (!m_device) {
        return std::nullopt;
    }

    const bool targetChanged = !m_texture || size != m_texture->size();
    if (!m_device || !ensureTarget(size)) {
        return std::nullopt;
    }
    return renderTo(m_texture.get(), layers, background, damage, targetChanged, std::move(acquireFence), targetColorDescription, outputColorPipeline, uploadManager, Timing::Enabled);
}

std::optional<VulkanCompositorRenderResult> VulkanCompositor::renderTo(VulkanTexture *target,
                                                                       std::span<const VulkanCompositorLayer> layers,
                                                                       const QColor &background,
                                                                       const Region &damage,
                                                                       FileDescriptor &&acquireFence,
                                                                       const std::shared_ptr<ColorDescription> &targetColorDescription,
                                                                       const ColorPipeline *outputColorPipeline,
                                                                       VulkanUploadManager *uploadManager)
{
    return renderTo(target, layers, background, damage, false, std::move(acquireFence), targetColorDescription, outputColorPipeline, uploadManager, Timing::Enabled);
}

std::optional<VulkanCompositorRenderResult> VulkanCompositor::renderTo(VulkanTexture *target,
                                                                       std::span<const VulkanCompositorLayer> layers,
                                                                       const QColor &background,
                                                                       const Region &damage,
                                                                       FileDescriptor &&acquireFence,
                                                                       const std::shared_ptr<ColorDescription> &targetColorDescription,
                                                                       const ColorPipeline *outputColorPipeline,
                                                                       VulkanUploadManager *uploadManager,
                                                                       Timing timing)
{
    return renderTo(target, layers, background, damage, false, std::move(acquireFence), targetColorDescription, outputColorPipeline, uploadManager, timing);
}

std::optional<VulkanCompositorRenderResult> VulkanCompositor::renderTo(VulkanTexture *target,
                                                                       std::span<const VulkanCompositorLayer> layers,
                                                                       const QColor &background,
                                                                       const Region &damage,
                                                                       bool forceFullDamage,
                                                                       FileDescriptor &&acquireFence,
                                                                       const std::shared_ptr<ColorDescription> &targetColorDescription,
                                                                       const ColorPipeline *outputColorPipeline,
                                                                       VulkanUploadManager *uploadManager,
                                                                       Timing timing)
{
    if (!m_device || !target || layers.size() > std::numeric_limits<uint32_t>::max()) {
        return std::nullopt;
    }

    if (!ensureResources(target->size(), layers.size()) || !acquireFrameResources()) {
        return std::nullopt;
    }
    const QSize size = target->size();

    std::vector<TextureBatch> batches(1);
    batches.front().firstLayer = 0;
    std::vector<GpuLayer> gpuLayers(layers.size());
    std::vector<GpuHotLayer> gpuHotLayers(layers.size());
    for (size_t i = 0; i < layers.size(); ++i) {
        const VulkanCompositorLayer &layer = layers[i];
        if (!layer.transform.isAffine() || !layer.textureTransform.isAffine()) {
            qCWarning(KWIN_VULKAN) << "Projective layer transforms are not supported yet";
            return std::nullopt;
        }
        bool invertible = false;
        const QTransform inverse = layer.transform.inverted(&invertible);
        if (!invertible) {
            return std::nullopt;
        }
        QRectF localRect = layer.rect.normalized();
        QRectF outputRect;
        if (layer.quadVertices) {
            QPointF localMinimum = layer.quadVertices->front();
            QPointF localMaximum = localMinimum;
            QPointF outputMinimum = layer.transform.map(localMinimum);
            QPointF outputMaximum = outputMinimum;
            for (size_t vertex = 1; vertex < layer.quadVertices->size(); ++vertex) {
                const QPointF local = (*layer.quadVertices)[vertex];
                const QPointF output = layer.transform.map(local);
                localMinimum.setX(std::min(localMinimum.x(), local.x()));
                localMinimum.setY(std::min(localMinimum.y(), local.y()));
                localMaximum.setX(std::max(localMaximum.x(), local.x()));
                localMaximum.setY(std::max(localMaximum.y(), local.y()));
                outputMinimum.setX(std::min(outputMinimum.x(), output.x()));
                outputMinimum.setY(std::min(outputMinimum.y(), output.y()));
                outputMaximum.setX(std::max(outputMaximum.x(), output.x()));
                outputMaximum.setY(std::max(outputMaximum.y(), output.y()));
            }
            localRect = QRectF(localMinimum, localMaximum).normalized();
            outputRect = QRectF(outputMinimum, outputMaximum).normalized();
        } else {
            outputRect = layer.transform.mapRect(localRect);
        }
        uint32_t flags = layer.clipRect ? 1 : 0;
        if (layer.roundedRect) {
            flags |= 1u << 1;
        }
        if (layer.outlineThickness > 0) {
            if (!layer.roundedRect) {
                qCWarning(KWIN_VULKAN) << "Vulkan outline layers require roundedRect";
                return std::nullopt;
            }
            flags |= 1u << 2;
        }
        if (layer.blendMode == VulkanBlendMode::DestinationOut) {
            flags |= 1u << 3;
        } else if (layer.blendMode == VulkanBlendMode::BackdropReplace) {
            flags |= 1u << 6;
        } else if (layer.blendMode == VulkanBlendMode::Additive) {
            flags |= 1u << 7;
        }
        const QColor modulation = layer.color.toRgb();
        const bool hasTexture = layer.texture || layer.texturePlaneCount > 0;
        const bool identityColorConversion = layer.colorDescription == targetColorDescription
            && layer.colorFilter == VulkanColorFilter::None
            && layer.brightness == 1.0
            && layer.saturation == 1.0
            && (!hasTexture
                || (modulation.redF() == 1.0
                    && modulation.greenF() == 1.0
                    && modulation.blueF() == 1.0));
        const bool transformColor = layer.blendMode != VulkanBlendMode::DestinationOut
            && layer.colorDescription && targetColorDescription
            && !identityColorConversion;
        if (transformColor) {
            flags |= 1u << 4;
        }
        if (layer.quadVertices) {
            flags |= 1u << 5;
        }
        const QRectF clipRect = layer.clipRect ? layer.clipRect->normalized() : QRectF{};
        if (layer.clipRect) {
            outputRect = outputRect.intersected(clipRect);
        }
        const uint32_t effectivePlaneCount = layer.texturePlaneCount == 0
            ? (layer.texture ? 1u : 0u)
            : layer.texturePlaneCount;
        const bool axisAlignedFastPath = !layer.quadVertices
            && !layer.roundedRect
            && layer.outlineThickness <= 0
            && layer.colorFilter != VulkanColorFilter::FractionalDebug
            && layer.transform.m12() == 0.0
            && layer.transform.m21() == 0.0;
        if (axisAlignedFastPath) {
            flags |= 1u << 8;
        }
        const bool guaranteedOpaque = axisAlignedFastPath
            && layer.blendMode == VulkanBlendMode::SourceOver
            && layer.colorFilter == VulkanColorFilter::None
            && layer.opacity >= 1.0
            && modulation.alphaF() >= 1.0
            && (layer.opaque || !hasTexture);
        if (guaranteedOpaque) {
            flags |= 1u << 13;
        }
        const bool simpleSourceOver = axisAlignedFastPath
            && layer.blendMode == VulkanBlendMode::SourceOver
            && layer.colorFilter == VulkanColorFilter::None
            && !transformColor
            && effectivePlaneCount <= 1
            && !layer.auxiliaryTexture;
        if (simpleSourceOver) {
            flags |= 1u << 9;
        }
        const bool colorManagedSourceOver = axisAlignedFastPath
            && layer.blendMode == VulkanBlendMode::SourceOver
            && layer.colorFilter == VulkanColorFilter::None
            && transformColor
            && effectivePlaneCount <= 1
            && !layer.auxiliaryTexture;
        if (colorManagedSourceOver) {
            flags |= 1u << 10;
        }
        GpuLayer &gpuLayer = gpuLayers[i];
        gpuLayer.colorFilterMatrixX = matrixRow(layer.colorFilterMatrix, 0);
        gpuLayer.colorFilterMatrixY = matrixRow(layer.colorFilterMatrix, 1);
        gpuLayer.colorFilterMatrixZ = matrixRow(layer.colorFilterMatrix, 2);
        gpuLayer.colorFilterParameters = {
            layer.colorFilterParameters.x(),
            layer.colorFilterParameters.y(),
            layer.colorFilterParameters.z(),
            layer.colorFilterParameters.w(),
        };
        gpuLayer.metadata[1] = uint32_t(layer.colorFilter);
        gpuLayer.rect = {float(outputRect.left()), float(outputRect.top()), float(outputRect.right()), float(outputRect.bottom())};
        gpuLayer.localRect = {float(localRect.left()), float(localRect.top()), float(localRect.right()), float(localRect.bottom())};
        gpuLayer.inverseTransformX = {float(inverse.m11()), float(inverse.m21()), float(inverse.dx()), 0.0f};
        gpuLayer.inverseTransformY = {float(inverse.m12()), float(inverse.m22()), float(inverse.dy()), 0.0f};
        gpuLayer.clipRect = {float(clipRect.left()), float(clipRect.top()), float(clipRect.right()), float(clipRect.bottom())};
        if (layer.colorFilter == VulkanColorFilter::FractionalDebug) {
            const std::array<QPointF, 4> vertices = layer.quadVertices.value_or(std::array<QPointF, 4>{
                localRect.topLeft(),
                localRect.topRight(),
                localRect.bottomRight(),
                localRect.bottomLeft(),
            });
            for (size_t vertex = 0; vertex < vertices.size(); ++vertex) {
                gpuLayer.colorFilterMatrixX[vertex] = fractionalVertex(layer.transform.map(vertices[vertex]));
            }
        }
        if (layer.quadVertices) {
            for (size_t vertex = 0; vertex < layer.quadVertices->size(); ++vertex) {
                gpuLayer.quadPositionX[vertex] = float((*layer.quadVertices)[vertex].x());
                gpuLayer.quadPositionY[vertex] = float((*layer.quadVertices)[vertex].y());
                gpuLayer.quadTextureX[vertex] = float(layer.quadTextureCoordinates[vertex].x());
                gpuLayer.quadTextureY[vertex] = float(layer.quadTextureCoordinates[vertex].y());
            }
        }
        const qreal xScale = std::hypot(layer.transform.m11(), layer.transform.m12());
        const qreal yScale = std::hypot(layer.transform.m21(), layer.transform.m22());
        const qreal antialiasScale = std::max(std::min(xScale, yScale), 0.0001);
        gpuLayer.effects = {
            float(std::max(layer.brightness, 0.0)),
            float(std::max(layer.saturation, 0.0)),
            float(std::max(layer.outlineThickness, 0.0)),
            float(antialiasScale),
        };
        if (transformColor) {
            const QMatrix4x4 matrix = layer.colorDescription->toOther(*targetColorDescription, layer.renderingIntent);
            gpuLayer.colorTransformX = matrixRow(matrix, 0);
            gpuLayer.colorTransformY = matrixRow(matrix, 1);
            gpuLayer.colorTransformZ = matrixRow(matrix, 2);
            const auto sourceParams = transferParameters(layer.colorDescription->transferFunction());
            const auto destinationParams = transferParameters(targetColorDescription->transferFunction());
            gpuLayer.transferParameters = {sourceParams[0], sourceParams[1], destinationParams[0], destinationParams[1]};
            const QMatrix4x4 toXYZ = targetColorDescription->containerColorimetry().toXYZ();
            gpuLayer.colorParameters = {
                toXYZ(1, 0),
                toXYZ(1, 1),
                toXYZ(1, 2),
                float(targetColorDescription->maxHdrLuminance().value_or(10'000)),
            };
            const QMatrix4x4 destinationToLms = targetColorDescription->containerColorimetry().toLMS();
            const QMatrix4x4 lmsToDestination = targetColorDescription->containerColorimetry().fromLMS();
            gpuLayer.destinationToLmsX = matrixRow(destinationToLms, 0);
            gpuLayer.destinationToLmsY = matrixRow(destinationToLms, 1);
            gpuLayer.destinationToLmsZ = matrixRow(destinationToLms, 2);
            gpuLayer.lmsToDestinationX = matrixRow(lmsToDestination, 0);
            gpuLayer.lmsToDestinationY = matrixRow(lmsToDestination, 1);
            gpuLayer.lmsToDestinationZ = matrixRow(lmsToDestination, 2);
            static const bool disableToneMapping = qEnvironmentVariableIntValue("KWIN_DISABLE_TONEMAPPING") == 1;
            const float maximumToneMappingLuminance = !disableToneMapping && layer.renderingIntent == RenderingIntent::Perceptual
                ? float(layer.colorDescription->maxHdrLuminance().value_or(layer.colorDescription->referenceLuminance())
                        * targetColorDescription->referenceLuminance() / layer.colorDescription->referenceLuminance())
                : float(targetColorDescription->maxHdrLuminance().value_or(10'000));
            gpuLayer.toneMappingParameters = {
                maximumToneMappingLuminance,
                float(targetColorDescription->referenceLuminance()),
                0.0f,
                0.0f,
            };
            gpuLayer.metadata[3] = uint32_t(layer.colorDescription->transferFunction().type)
                | (uint32_t(targetColorDescription->transferFunction().type) << 8);
        }
        if (layer.roundedRect) {
            const QRectF roundedRect = layer.roundedRect->normalized();
            gpuLayer.roundedRect = {
                float(roundedRect.left()),
                float(roundedRect.top()),
                float(roundedRect.right()),
                float(roundedRect.bottom()),
            };
            gpuLayer.cornerRadii = {
                std::max(layer.cornerRadii.x(), 0.0f),
                std::max(layer.cornerRadii.y(), 0.0f),
                std::max(layer.cornerRadii.z(), 0.0f),
                std::max(layer.cornerRadii.w(), 0.0f),
            };
        }
        gpuLayer.metadata[2] = flags;
        if (layer.blendMode == VulkanBlendMode::DestinationOut) {
            gpuLayer.color = {0.0f, 0.0f, 0.0f, float(std::clamp(layer.opacity, 0.0, 1.0))};
        } else if (layer.texture || layer.texturePlaneCount > 0) {
            std::array<VulkanTexture *, 3> planeTextures = layer.texturePlanes;
            uint32_t planeCount = layer.texturePlaneCount;
            if (planeCount == 0) {
                planeTextures[0] = layer.texture;
                planeCount = 1;
            }
            if (planeCount > planeTextures.size() || !planeTextures[0]) {
                return std::nullopt;
            }
            std::array<VulkanTexture *, 4> sampledTextures{};
            std::copy_n(planeTextures.begin(), planeCount, sampledTextures.begin());
            uint32_t sampledTextureCount = planeCount;
            if (layer.auxiliaryTexture) {
                sampledTextures[sampledTextureCount++] = layer.auxiliaryTexture;
            }
            TextureBatch *batch = &batches.back();
            uint32_t newTextureCount = 0;
            for (uint32_t slot = 0; slot < sampledTextureCount; ++slot) {
                if (!sampledTextures[slot]) {
                    return std::nullopt;
                }
                const bool alreadyPending = std::find(sampledTextures.begin(),
                                                      sampledTextures.begin() + slot,
                                                      sampledTextures[slot])
                    != sampledTextures.begin() + slot;
                if (!alreadyPending && !std::ranges::contains(batch->textures, sampledTextures[slot])) {
                    ++newTextureCount;
                }
            }
            if (batch->textures.size() + newTextureCount > MaximumTextureCount) {
                batch->lastLayer = uint32_t(i);
                batches.push_back(TextureBatch{
                    .firstLayer = uint32_t(i),
                });
                batch = &batches.back();
            }
            for (uint32_t slot = 0; slot < sampledTextureCount; ++slot) {
                auto textureIt = std::ranges::find(batch->textures, sampledTextures[slot]);
                if (textureIt == batch->textures.end()) {
                    batch->textures.push_back(sampledTextures[slot]);
                    textureIt = std::prev(batch->textures.end());
                }
                gpuLayer.textureIndices[slot] = std::distance(batch->textures.begin(), textureIt);
            }
            const QSize textureSize = planeTextures[0]->size();
            if (textureSize.isEmpty()) {
                return std::nullopt;
            }
            const QRectF source = layer.sourceRect.isEmpty()
                ? QRectF(QPointF(0, 0), QSizeF(textureSize))
                : layer.sourceRect;
            gpuLayer.sourceRect = {
                float(source.left() / textureSize.width()),
                float(source.top() / textureSize.height()),
                float(source.right() / textureSize.width()),
                float(source.bottom() / textureSize.height()),
            };
            if (layer.quadVertices) {
                gpuLayer.textureCoordinatesX = {float(layer.textureTransform.m11()), float(layer.textureTransform.m21()), float(layer.textureTransform.dx()), 0.0f};
                gpuLayer.textureCoordinatesY = {float(layer.textureTransform.m12()), float(layer.textureTransform.m22()), float(layer.textureTransform.dy()), 0.0f};
            } else if (localRect.isEmpty()) {
                gpuLayer.textureCoordinatesX = {};
                gpuLayer.textureCoordinatesY = {};
            } else {
                const QRectF normalizedSource(gpuLayer.sourceRect[0],
                                              gpuLayer.sourceRect[1],
                                              gpuLayer.sourceRect[2] - gpuLayer.sourceRect[0],
                                              gpuLayer.sourceRect[3] - gpuLayer.sourceRect[1]);
                const auto outputToTexture = [&](const QPointF &outputPosition) {
                    const QPointF localPosition = inverse.map(outputPosition);
                    const QPointF normalizedPosition((localPosition.x() - localRect.left()) / localRect.width(),
                                                     (localPosition.y() - localRect.top()) / localRect.height());
                    const QPointF transformedPosition = layer.textureTransform.map(normalizedPosition);
                    return QPointF(normalizedSource.left() + transformedPosition.x() * normalizedSource.width(),
                                   normalizedSource.top() + transformedPosition.y() * normalizedSource.height());
                };
                const QPointF origin = outputToTexture(QPointF(0, 0));
                const QPointF xUnit = outputToTexture(QPointF(1, 0));
                const QPointF yUnit = outputToTexture(QPointF(0, 1));
                gpuLayer.textureCoordinatesX = {
                    float(xUnit.x() - origin.x()),
                    float(yUnit.x() - origin.x()),
                    float(origin.x()),
                    0.0f,
                };
                gpuLayer.textureCoordinatesY = {
                    float(xUnit.y() - origin.y()),
                    float(yUnit.y() - origin.y()),
                    float(origin.y()),
                    0.0f,
                };
            }
            gpuLayer.color = {
                float(modulation.redF()),
                float(modulation.greenF()),
                float(modulation.blueF()),
                float(modulation.alphaF() * std::clamp(layer.opacity, 0.0, 1.0)),
            };
            gpuLayer.metadata[0] = planeCount;
            if (planeCount > 1) {
                const QMatrix4x4 yuvMatrix = layer.colorDescription ? layer.colorDescription->yuvMatrix() : QMatrix4x4{};
                gpuLayer.yuvTransformX = matrixRow(yuvMatrix, 0);
                gpuLayer.yuvTransformY = matrixRow(yuvMatrix, 1);
                gpuLayer.yuvTransformZ = matrixRow(yuvMatrix, 2);
            }
        } else {
            gpuLayer.color = premultiplied(layer.color, layer.opacity);
        }
        GpuHotLayer &gpuHotLayer = gpuHotLayers[i];
        gpuHotLayer.rect = gpuLayer.rect;
        gpuHotLayer.color = gpuLayer.color;
        gpuHotLayer.textureCoordinatesX = gpuLayer.textureCoordinatesX;
        gpuHotLayer.textureCoordinatesY = gpuLayer.textureCoordinatesY;
        gpuHotLayer.effects = gpuLayer.effects;
        gpuHotLayer.textureIndices = gpuLayer.textureIndices;
        gpuHotLayer.metadata = gpuLayer.metadata;
    }
    batches.back().lastLayer = uint32_t(layers.size());
    const uint32_t batchCount = uint32_t(batches.size());
    if (!ensureDescriptorSets(batchCount)
        || !updateBufferDescriptors(batchCount)
        || !updateTargetDescriptor(target, batchCount)) {
        return std::nullopt;
    }

    std::vector<vk::ImageMemoryBarrier2> inputAcquireBarriers;
    std::vector<vk::ImageMemoryBarrier2> inputReleaseBarriers;
    if (!updateTextureDescriptors(batches, inputAcquireBarriers, inputReleaseBarriers)) {
        return std::nullopt;
    }
    const bool applyOutputLut = outputColorPipeline && !outputColorPipeline->isIdentity();
    if (applyOutputLut && !updateOutputLut(*outputColorPipeline)) {
        return std::nullopt;
    }
    const bool useSimpleCompositePipeline = !applyOutputLut
        && std::ranges::all_of(gpuHotLayers, [](const GpuHotLayer &layer) {
        return (layer.metadata[2] & (1u << 9)) != 0u;
    });
    const bool useColorCompositePipeline = !applyOutputLut
        && std::ranges::all_of(gpuHotLayers, [](const GpuHotLayer &layer) {
        return (layer.metadata[2] & (1u << 10)) != 0u;
    });
    if (!layers.empty()) {
        if (!useSimpleCompositePipeline) {
            std::memcpy(m_currentFrame->layerData, gpuLayers.data(), sizeof(GpuLayer) * layers.size());
        }

        std::memcpy(m_currentFrame->hotLayerData, gpuHotLayers.data(), sizeof(GpuHotLayer) * layers.size());
    }

    const uint32_t tilesWide = (size.width() + TileSize - 1) / TileSize;
    const uint32_t tilesHigh = (size.height() + TileSize - 1) / TileSize;
    const uint32_t binsWide = (tilesWide + TileSize - 1) / TileSize;
    const uint32_t binsHigh = (tilesHigh + TileSize - 1) / TileSize;
    const uint32_t maskWordCount = (uint32_t(layers.size()) + 31) / 32;
    const Region effectiveDamage = forceFullDamage
        ? Region(0, 0, size.width(), size.height())
        : damage.intersected(Rect(0, 0, size.width(), size.height()));
    std::vector<uint8_t> markedTiles(size_t(tilesWide) * tilesHigh);
    std::vector<uint32_t> dirtyTiles;
    for (const Rect &rect : effectiveDamage.rects()) {
        const uint32_t firstTileX = uint32_t(rect.x()) / TileSize;
        const uint32_t firstTileY = uint32_t(rect.y()) / TileSize;
        const uint32_t lastTileX = (uint32_t(rect.x() + rect.width()) + TileSize - 1) / TileSize;
        const uint32_t lastTileY = (uint32_t(rect.y() + rect.height()) + TileSize - 1) / TileSize;
        for (uint32_t tileY = firstTileY; tileY < lastTileY; ++tileY) {
            for (uint32_t tileX = firstTileX; tileX < lastTileX; ++tileX) {
                const uint32_t tileIndex = tileY * tilesWide + tileX;
                if (!markedTiles[tileIndex]) {
                    markedTiles[tileIndex] = true;
                    dirtyTiles.push_back(tileX | (tileY << 16));
                }
            }
        }
    }
    const vk::Extent3D maximumGroupCount = m_device->maximumComputeWorkGroupCount();
    const uint32_t compositeGroupCountX = dirtyTiles.empty()
        ? 0
        : std::min<uint64_t>(dirtyTiles.size(), maximumGroupCount.width);
    const uint64_t compositeGroupCountY64 = compositeGroupCountX == 0
        ? 0
        : (dirtyTiles.size() + compositeGroupCountX - 1) / compositeGroupCountX;
    if (compositeGroupCountY64 > maximumGroupCount.height) {
        qCWarning(KWIN_VULKAN) << "Vulkan compositor damage exceeds the device's compute dispatch limits";
        return std::nullopt;
    }
    const uint32_t compositeGroupCountY = uint32_t(compositeGroupCountY64);
    if (!dirtyTiles.empty()) {
        std::memcpy(m_currentFrame->dirtyTileData, dirtyTiles.data(), sizeof(uint32_t) * dirtyTiles.size());
    }
    PushConstants pushConstants{
        .layerCount = uint32_t(layers.size()),
        .tilesWide = tilesWide,
        .flags = std::ranges::any_of(layers, [](const VulkanCompositorLayer &layer) {
        return layer.blendMode != VulkanBlendMode::SourceOver;
    })
            ? 1u
            : 0u,
        .dirtyTileCount = uint32_t(dirtyTiles.size()),
        .background = premultiplied(background),
        .firstLayer = 0,
        .lastLayer = uint32_t(layers.size()),
        .padding = {uint32_t(size.width()), uint32_t(size.height())},
        .outputLutParameters = applyOutputLut ? std::array<float, 4>{float(outputColorPipeline->inputRange.min), float(outputColorPipeline->inputRange.max), float(OutputLutEdgeSize), 0.0f} : std::array<float, 4>{},
    };

    std::vector<VulkanUploadManager *> pendingUploadManagers;
    for (const TextureBatch &batch : batches) {
        for (VulkanTexture *texture : batch.textures) {
            VulkanUploadManager *manager = texture->pendingUploadManager();
            if (manager && !std::ranges::contains(pendingUploadManagers, manager)) {
                pendingUploadManagers.push_back(manager);
            }
        }
    }
    if (uploadManager && uploadManager->hasPendingUploads()
        && !std::ranges::contains(pendingUploadManagers, uploadManager)) {
        pendingUploadManagers.push_back(uploadManager);
    }

    auto commandBuffer = m_device->createComputeCommandBuffer();
    if (!*commandBuffer || commandBuffer.begin(vk::CommandBufferBeginInfo{vk::CommandBufferUsageFlagBits::eOneTimeSubmit}) != vk::Result::eSuccess) {
        return std::nullopt;
    }
    for (VulkanUploadManager *manager : pendingUploadManagers) {
        if (!manager->record(commandBuffer)) {
            for (VulkanUploadManager *pending : pendingUploadManagers) {
                pending->submissionFailed();
            }
            return std::nullopt;
        }
    }

    const vk::BufferMemoryBarrier2 hostBarrier{
        vk::PipelineStageFlagBits2::eHost,
        vk::AccessFlagBits2::eHostWrite,
        vk::PipelineStageFlagBits2::eComputeShader,
        vk::AccessFlagBits2::eShaderRead,
        vk::QueueFamilyIgnored,
        vk::QueueFamilyIgnored,
        *m_currentFrame->layerBuffer,
        0,
        vk::WholeSize,
    };
    const vk::BufferMemoryBarrier2 hotLayerHostBarrier{
        vk::PipelineStageFlagBits2::eHost,
        vk::AccessFlagBits2::eHostWrite,
        vk::PipelineStageFlagBits2::eComputeShader,
        vk::AccessFlagBits2::eShaderRead,
        vk::QueueFamilyIgnored,
        vk::QueueFamilyIgnored,
        *m_currentFrame->hotLayerBuffer,
        0,
        vk::WholeSize,
    };
    const vk::BufferMemoryBarrier2 dirtyTileHostBarrier{
        vk::PipelineStageFlagBits2::eHost,
        vk::AccessFlagBits2::eHostWrite,
        vk::PipelineStageFlagBits2::eComputeShader,
        vk::AccessFlagBits2::eShaderRead,
        vk::QueueFamilyIgnored,
        vk::QueueFamilyIgnored,
        *m_currentFrame->dirtyTileBuffer,
        0,
        vk::WholeSize,
    };
    const vk::BufferMemoryBarrier2 outputLutHostBarrier{
        vk::PipelineStageFlagBits2::eHost,
        vk::AccessFlagBits2::eHostWrite,
        vk::PipelineStageFlagBits2::eComputeShader,
        vk::AccessFlagBits2::eShaderRead,
        vk::QueueFamilyIgnored,
        vk::QueueFamilyIgnored,
        *m_currentFrame->outputLutBuffer,
        0,
        vk::WholeSize,
    };
    const std::array hostBarriers{hostBarrier, hotLayerHostBarrier, dirtyTileHostBarrier, outputLutHostBarrier};
    commandBuffer.pipelineBarrier2(vk::DependencyInfo{{}, {}, hostBarriers, inputAcquireBarriers});

    auto preprocessQuery = timing == Timing::Enabled
        ? VulkanRenderTimeQuery::begin(m_device,
                                       commandBuffer,
                                       m_device->computeQueueFamily(),
                                       vk::PipelineStageFlagBits2::eAllCommands)
        : nullptr;
    commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_pipelineLayout, 0, *m_currentFrame->descriptorSets.front(), {});
    commandBuffer.pushConstants(m_pipelineLayout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(pushConstants), &pushConstants);
    constexpr uint32_t PrefixScanLayerThreshold = 32;
    if (!dirtyTiles.empty() && maskWordCount != 0 && layers.size() <= PrefixScanLayerThreshold) {
        commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, m_bruteForceMaskPipeline);
        commandBuffer.dispatch((dirtyTiles.size() + 63) / 64, 1, 1);
    } else if (!dirtyTiles.empty() && maskWordCount != 0) {
        commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, m_preprocessPipeline);
        commandBuffer.dispatch(binsWide, binsHigh, maskWordCount);
        const vk::BufferMemoryBarrier2 prefixBarrier{
            vk::PipelineStageFlagBits2::eComputeShader,
            vk::AccessFlagBits2::eShaderWrite,
            vk::PipelineStageFlagBits2::eComputeShader,
            vk::AccessFlagBits2::eShaderRead,
            vk::QueueFamilyIgnored,
            vk::QueueFamilyIgnored,
            *m_currentFrame->prefixBuffer,
            0,
            vk::WholeSize,
        };
        commandBuffer.pipelineBarrier2(vk::DependencyInfo{{}, {}, prefixBarrier, {}});

        commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, m_prefixPropagatePipeline);
        const uint32_t propagateJobCount = maskWordCount * (tilesWide + tilesHigh);
        commandBuffer.dispatch((propagateJobCount + 63) / 64, 1, 1);
        const vk::BufferMemoryBarrier2 carryBarrier{
            vk::PipelineStageFlagBits2::eComputeShader,
            vk::AccessFlagBits2::eShaderWrite,
            vk::PipelineStageFlagBits2::eComputeShader,
            vk::AccessFlagBits2::eShaderRead,
            vk::QueueFamilyIgnored,
            vk::QueueFamilyIgnored,
            *m_currentFrame->carryBuffer,
            0,
            vk::WholeSize,
        };
        commandBuffer.pipelineBarrier2(vk::DependencyInfo{{}, {}, carryBarrier, {}});

        commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, m_prefixFinalizePipeline);
        commandBuffer.dispatch(binsWide, binsHigh, 1);
    }
    const vk::BufferMemoryBarrier2 tileBarrier{
        vk::PipelineStageFlagBits2::eComputeShader,
        vk::AccessFlagBits2::eShaderWrite,
        vk::PipelineStageFlagBits2::eComputeShader,
        vk::AccessFlagBits2::eShaderRead,
        vk::QueueFamilyIgnored,
        vk::QueueFamilyIgnored,
        *m_currentFrame->tileBuffer,
        0,
        vk::WholeSize,
    };
    const bool externalTarget = target->isExternal();
    const vk::ImageMemoryBarrier2 imageWriteBarrier{
        externalTarget ? vk::PipelineStageFlagBits2::eNone : vk::PipelineStageFlagBits2::eAllCommands,
        externalTarget ? vk::AccessFlags2{} : vk::AccessFlags2(vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite),
        vk::PipelineStageFlagBits2::eComputeShader,
        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
        vk::ImageLayout::eGeneral,
        vk::ImageLayout::eGeneral,
        externalTarget ? vk::QueueFamilyExternal : vk::QueueFamilyIgnored,
        externalTarget ? m_device->computeQueueFamily() : vk::QueueFamilyIgnored,
        target->handle(),
        vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    commandBuffer.pipelineBarrier2(vk::DependencyInfo{{}, {}, tileBarrier, imageWriteBarrier});
    if (preprocessQuery) {
        preprocessQuery->end(commandBuffer);
    }

    auto compositeQuery = timing == Timing::Enabled
        ? VulkanRenderTimeQuery::begin(m_device,
                                       commandBuffer,
                                       m_device->computeQueueFamily(),
                                       vk::PipelineStageFlagBits2::eAllCommands)
        : nullptr;
    const vk::Pipeline compositePipeline = useSimpleCompositePipeline
        ? (layers.size() <= 2 ? *m_shallowCompositePipeline : *m_simpleCompositePipeline)
        : useColorCompositePipeline
        ? *m_colorCompositePipeline
        : *m_compositePipeline;
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, compositePipeline);
    for (uint32_t batchIndex = 0; batchIndex < batchCount; ++batchIndex) {
        const TextureBatch &batch = batches[batchIndex];
        pushConstants.firstLayer = batch.firstLayer;
        pushConstants.lastLayer = batch.lastLayer;
        if (batchIndex == 0) {
            pushConstants.flags &= ~(1u << 1);
        } else {
            pushConstants.flags |= 1u << 1;
        }
        if (applyOutputLut && batchIndex + 1 == batchCount) {
            pushConstants.flags |= 1u << 2;
        } else {
            pushConstants.flags &= ~(1u << 2);
        }
        commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                                         m_pipelineLayout,
                                         0,
                                         *m_currentFrame->descriptorSets[batchIndex],
                                         {});
        commandBuffer.pushConstants(m_pipelineLayout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(pushConstants), &pushConstants);
        if (!dirtyTiles.empty()) {
            commandBuffer.dispatch(compositeGroupCountX, compositeGroupCountY, 1);
        }
        if (batchIndex + 1 < batchCount) {
            const vk::ImageMemoryBarrier2 batchBarrier{
                vk::PipelineStageFlagBits2::eComputeShader,
                vk::AccessFlagBits2::eShaderWrite,
                vk::PipelineStageFlagBits2::eComputeShader,
                vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                vk::ImageLayout::eGeneral,
                vk::ImageLayout::eGeneral,
                vk::QueueFamilyIgnored,
                vk::QueueFamilyIgnored,
                target->handle(),
                vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            };
            commandBuffer.pipelineBarrier2(vk::DependencyInfo{{}, {}, {}, batchBarrier});
        }
    }
    const vk::ImageMemoryBarrier2 targetReleaseBarrier{
        vk::PipelineStageFlagBits2::eComputeShader,
        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
        externalTarget ? vk::PipelineStageFlagBits2::eNone : vk::PipelineStageFlagBits2::eTransfer,
        externalTarget ? vk::AccessFlags2{} : vk::AccessFlags2(vk::AccessFlagBits2::eTransferRead),
        vk::ImageLayout::eGeneral,
        vk::ImageLayout::eGeneral,
        externalTarget ? m_device->computeQueueFamily() : vk::QueueFamilyIgnored,
        externalTarget ? vk::QueueFamilyExternal : vk::QueueFamilyIgnored,
        target->handle(),
        vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    inputReleaseBarriers.push_back(targetReleaseBarrier);
    commandBuffer.pipelineBarrier2(vk::DependencyInfo{{}, {}, {}, inputReleaseBarriers});
    if (compositeQuery) {
        compositeQuery->end(commandBuffer);
    }
    if (commandBuffer.end() != vk::Result::eSuccess) {
        for (VulkanUploadManager *manager : pendingUploadManagers) {
            manager->submissionFailed();
        }
        return std::nullopt;
    }
    auto completionFence = m_device->submitCompute(std::move(commandBuffer), std::move(acquireFence));
    if (!completionFence) {
        for (VulkanUploadManager *manager : pendingUploadManagers) {
            manager->submissionFailed();
        }
        return std::nullopt;
    }
    for (VulkanUploadManager *manager : pendingUploadManagers) {
        manager->submitted(*completionFence);
    }
    m_currentFrame->completionFence = completionFence->duplicate();

    VulkanCompositorRenderResult result;
    result.texture = target;
    result.completionFence = std::move(*completionFence);
    result.preprocessTime = std::move(preprocessQuery);
    result.compositeTime = std::move(compositeQuery);
    return result;
}

VulkanTexture *VulkanCompositor::texture() const
{
    return m_texture.get();
}

QSize VulkanCompositor::size() const
{
    return m_size;
}

void VulkanCompositor::releaseResources()
{
    m_currentFrame = nullptr;
    for (FrameResources &frame : m_frames) {
        frame.completionFence = FileDescriptor{};
        frame.unmapHostMemory();
        frame.tileBuffer.clear();
        frame.tileMemory.clear();
        frame.prefixBuffer.clear();
        frame.prefixMemory.clear();
        frame.carryBuffer.clear();
        frame.carryMemory.clear();
        frame.dirtyTileBuffer.clear();
        frame.dirtyTileMemory.clear();
        frame.outputLutBuffer.clear();
        frame.outputLutMemory.clear();
        frame.outputColorPipeline.reset();
        frame.layerBuffer.clear();
        frame.layerMemory.clear();
        frame.hotLayerBuffer.clear();
        frame.hotLayerMemory.clear();
        frame.descriptorSets.clear();
        frame.descriptorPool.clear();
        frame.descriptorCapacity = 0;
    }
    m_fallbackTexture.reset();
    m_texture.reset();
    m_sampler.clear();
    m_preprocessPipeline.clear();
    m_bruteForceMaskPipeline.clear();
    m_prefixPropagatePipeline.clear();
    m_prefixFinalizePipeline.clear();
    m_compositePipeline.clear();
    m_colorCompositePipeline.clear();
    m_simpleCompositePipeline.clear();
    m_shallowCompositePipeline.clear();
    m_pipelineLayout.clear();
    m_descriptorSetLayout.clear();
    m_size = {};
    m_layerCapacity = 0;
}

} // namespace KWin
