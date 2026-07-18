/*
    SPDX-FileCopyrightText: 2026 Diego Gomez <diego.gomez2005@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "kwin_wayland_test.h"

#include "core/colorpipeline.h"
#include "core/drmdevice.h"
#include "core/gpumanager.h"
#include "core/graphicsbuffer.h"
#include "core/iccprofile.h"
#include "core/renderdevice.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effect.h"
#include "opengl/eglcontext.h"
#include "opengl/glframebuffer.h"
#include "opengl/gltexture.h"
#include "scene/atlas.h"
#include "scene/borderoutline.h"
#include "scene/imageitem.h"
#include "scene/itemrenderer_opengl.h"
#include "scene/itemrenderer_qpainter.h"
#include "scene/itemrenderer_vulkan.h"
#include "scene/outlinedborderitem.h"
#include "scene/rootitem.h"
#include "scene/scene.h"
#include "scene/vulkan/ninepatch.h"
#include "scene/vulkan/texture.h"
#include "vulkan/vulkan_compositor.h"
#include "vulkan/vulkan_device.h"
#include "vulkan/vulkan_render_time_query.h"
#include "vulkan/vulkan_rendertarget.h"
#include "vulkan/vulkan_swapchain.h"
#include "vulkan/vulkan_texture.h"
#include "wayland_server.h"

#include <QPainter>
#include <QPainterPath>

#include <drm_fourcc.h>

namespace KWin
{

class VulkanTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void testAllocateImage();
    void testUploadPattern();
    void testDeferredArgbUpload();
    void testDeferredXrgbUpload();
    void testUpdateRegion();
    void testUpdateCorners();
    void testComputeQueue();
    void testSwapchainBufferAge();
    void testTimestampQueryWait();
    void testTimestampQueriesDisabled();
    void testComputeSolidScenes_data();
    void testComputeSolidScenes();
    void testComputeTexturedLayers();
    void testComputeDescriptorBatches();
    void testComputeQuadGeometry();
    void testComputeTransformsAndClipping();
    void testComputeDamageTiles();
    void testComputeLargeDispatch();
    void testComputeBrightnessAndSaturation();
    void testComputeColorConversion();
    void testComputeHdrToneMapping();
    void testComputeOutputColorPipeline();
    void testComputeColorFilters();
    void testComputeFractionalDebug();
    void testComputeZoomFilters();
    void testComputeScreenTransformCrossFade();
    void testComputeIccOutput_data();
    void testComputeIccOutput();
    void testComputePlanarYuv();
    void testComputeRoundedGeometry();
    void testComputeDestinationOut();
    void testDecorationAtlasNullSprites();
    void testNinePatchUpload();
    void testHighPrecisionIntermediate();
    void testNativeRenderTarget();
    void testItemRendererExactDamage();
    void testNativeTargetTransforms();
    void testItemRendererPainterOverlay();
    void testItemRendererBackdropBlur();
    void testItemRendererNestedTarget();
    void testItemRendererFractionalDebug();
    void testItemRendererScene();
    void testItemRendererWindowTransform();
    void testDeviceLossRecovery();

private:
    RenderDevice *m_renderDevice = nullptr;
    VulkanDevice *m_device = nullptr;
    std::shared_ptr<EglContext> m_glContext;
};

class TestScene : public Scene
{
public:
    TestScene()
        : m_root(std::make_unique<RootItem>(this))
    {
    }

    RootItem *root() const
    {
        return m_root.get();
    }

    void attachRenderer(std::unique_ptr<ItemRenderer> &&renderer) override
    {
        releaseResources(m_root.get());
        m_renderer = std::move(renderer);
    }

    void detachRenderer() override
    {
        releaseResources(m_root.get());
        m_renderer.reset();
    }

    QList<Item *> layerCandidates(ssize_t maxTotalCount) const override
    {
        return {m_root.get()};
    }

    void prePaint(SceneView *view, OutputFrame *frame) override
    {
    }

    Region collectDamage() override
    {
        return {};
    }

    void paint(const RenderTarget &renderTarget, const QPoint &deviceOffset, const Region &deviceRegion) override
    {
    }

    void postPaint() override
    {
    }

    void frame(SceneView *delegate, OutputFrame *frame) override
    {
    }

    double desiredHdrHeadroom() const override
    {
        return 1.0;
    }

private:
    std::unique_ptr<RootItem> m_root;
};

static QImage referenceImage(const QSize &size, const QColor &background, std::span<const VulkanSolidLayer> layers)
{
    QImage image(size, QImage::Format_RGBA8888_Premultiplied);
    image.fill(background);
    QPainter painter(&image);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    for (const VulkanSolidLayer &layer : layers) {
        painter.fillRect(layer.rect, layer.color);
    }
    return image;
}

static int maximumChannelDifference(const QImage &left, const QImage &right)
{
    if (left.size() != right.size() || left.format() != right.format()) {
        return std::numeric_limits<int>::max();
    }
    int maximumDifference = 0;
    for (qsizetype i = 0; i < left.sizeInBytes(); ++i) {
        maximumDifference = std::max(maximumDifference, std::abs(int(left.constBits()[i]) - int(right.constBits()[i])));
    }
    return maximumDifference;
}

void VulkanTest::initTestCase()
{
    qRegisterMetaType<KWin::Window *>();
    QVERIFY(waylandServer()->init(qAppName()));
    kwinApp()->start();

    const auto &renderDevices = GpuManager::self()->renderDevices();
    for (const auto &dev : renderDevices) {
        if (dev->vulkanDevice()) {
            m_renderDevice = dev.get();
            m_device = dev->vulkanDevice();
            break;
        }
    }
    if (!m_device) {
        QSKIP("No Vulkan device available");
    }
    m_glContext = m_renderDevice->eglContext();
}

void VulkanTest::testAllocateImage()
{
    auto texture = VulkanTexture::allocate(m_device, vk::Format::eR8G8B8A8Unorm, QSize(64, 64),
                                           vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled);
    QVERIFY(texture != nullptr);
    QCOMPARE(texture->size(), QSize(64, 64));
    QCOMPARE(texture->format(), vk::Format::eR8G8B8A8Unorm);
}

void VulkanTest::testUploadPattern()
{
    const QSize size(7, 13);
    QImage source(size, QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            if (y < size.height() / 2) {
                source.setPixelColor(x, y, QColor(255, 0, 0, 255));
            } else {
                source.setPixelColor(x, y, QColor(0, 0, 255, 255));
            }
        }
    }

    auto texture = VulkanTexture::upload(m_device, source,
                                         vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled);
    QVERIFY(texture != nullptr);
    QCOMPARE(texture->size(), size);

    QImage downloaded = texture->download();
    QVERIFY(!downloaded.isNull());
    QCOMPARE(downloaded.size(), size);

    QColor topPixel(downloaded.pixel(3, 2));
    QCOMPARE(topPixel.red(), 255);
    QCOMPARE(topPixel.blue(), 0);

    QColor bottomPixel(downloaded.pixel(3, 10));
    QCOMPARE(bottomPixel.red(), 0);
    QCOMPARE(bottomPixel.blue(), 255);
}

void VulkanTest::testDeferredArgbUpload()
{
#if Q_BYTE_ORDER != Q_LITTLE_ENDIAN
    QSKIP("Native ARGB32 upload is currently little-endian only");
#endif
    const QSize size(17, 11);
    QImage source(size, QImage::Format_ARGB32_Premultiplied);
    source.fill(QColor(30, 80, 210, 190));
    source.setPixelColor(2, 3, QColor(240, 20, 60, 255));

    const auto format = VulkanTexture::qImageToVulkanFormat(source.format());
    QVERIFY(format.has_value());
    QCOMPARE(*format, vk::Format::eB8G8R8A8Unorm);
    auto texture = VulkanTexture::allocate(m_device,
                                           *format,
                                           size,
                                           vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
                                           VulkanQueueRole::Compute,
                                           VulkanTexture::qImageToComponentMapping(source.format()));
    QVERIFY(texture);

    VulkanUploadManager uploads(m_device);
    QVERIFY(uploads.upload(texture.get(), source, Region(0, 0, size.width(), size.height())));
    auto compositor = VulkanCompositor::create(m_device);
    QVERIFY(compositor);
    const std::array layers{VulkanCompositorLayer{
        .rect = QRectF(QPointF(), QSizeF(size)),
        .texture = texture.get(),
    }};
    auto initial = compositor->render(size, layers, Qt::transparent, Region::infinite(), nullptr, nullptr, {}, &uploads);
    QVERIFY(initial);
    QImage actual = initial->texture->download();
    QCOMPARE(actual, source.convertToFormat(QImage::Format_RGBA8888_Premultiplied));

    QImage updated = source;
    updated.setPixelColor(8, 5, QColor(15, 230, 90, 128));
    const Region damage(8, 5, 1, 1);
    QVERIFY(uploads.upload(texture.get(), updated, damage));
    auto changed = compositor->render(size, layers, Qt::transparent, damage, nullptr, nullptr, {}, &uploads);
    QVERIFY(changed);
    actual = changed->texture->download();
    QCOMPARE(actual, updated.convertToFormat(QImage::Format_RGBA8888_Premultiplied));
}

void VulkanTest::testDeferredXrgbUpload()
{
#if Q_BYTE_ORDER != Q_LITTLE_ENDIAN
    QSKIP("Native RGB32 upload is currently little-endian only");
#endif
    const QSize size(7, 5);
    QImage source(size, QImage::Format_RGB32);
    for (int y = 0; y < size.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(source.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            // XRGB's unused byte is deliberately zero. The image-view swizzle
            // must make the sampled surface opaque without rewriting pixels.
            line[x] = (uint32_t(20 + x * 7) << 16) | (uint32_t(40 + y * 11) << 8) | uint32_t(180 - x * 9);
        }
    }
    const auto format = VulkanTexture::qImageToVulkanFormat(source.format());
    QVERIFY(format.has_value());
    QCOMPARE(*format, vk::Format::eB8G8R8A8Unorm);
    const vk::ComponentMapping mapping = VulkanTexture::qImageToComponentMapping(source.format());
    QCOMPARE(mapping.a, vk::ComponentSwizzle::eOne);
    auto texture = VulkanTexture::allocate(m_device,
                                           *format,
                                           size,
                                           vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
                                           VulkanQueueRole::Compute,
                                           mapping);
    QVERIFY(texture);

    VulkanUploadManager uploads(m_device);
    QVERIFY(uploads.upload(texture.get(), source, Region(0, 0, size.width(), size.height())));
    auto compositor = VulkanCompositor::create(m_device);
    QVERIFY(compositor);
    const std::array layers{VulkanCompositorLayer{
        .rect = QRectF(QPointF(), QSizeF(size)),
        .texture = texture.get(),
    }};
    auto result = compositor->render(size, layers, Qt::transparent, Region::infinite(), nullptr, nullptr, {}, &uploads);
    QVERIFY(result);
    const QImage actual = result->texture->download();
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            const QColor pixel = actual.pixelColor(x, y);
            QCOMPARE(pixel.alpha(), 255);
            QCOMPARE(pixel.red(), 20 + x * 7);
            QCOMPARE(pixel.green(), 40 + y * 11);
            QCOMPARE(pixel.blue(), 180 - x * 9);
        }
    }
}

void VulkanTest::testUpdateRegion()
{
    QImage source(64, 64, QImage::Format_RGBA8888_Premultiplied);
    source.fill(QColor(0, 255, 0, 255));

    auto texture = VulkanTexture::upload(m_device, source,
                                         vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled);
    QVERIFY(texture != nullptr);

    QImage updateSource(64, 64, QImage::Format_RGBA8888_Premultiplied);
    updateSource.fill(QColor(0, 255, 0, 255));
    for (int y = 24; y < 40; ++y) {
        for (int x = 24; x < 40; ++x) {
            updateSource.setPixelColor(x, y, QColor(255, 0, 0, 255));
        }
    }

    Region dirtyRegion(Rect(24, 24, 16, 16));
    QVERIFY(texture->update(updateSource, dirtyRegion));

    QImage downloaded = texture->download();
    QVERIFY(!downloaded.isNull());

    QColor corner(downloaded.pixel(0, 0));
    QCOMPARE(corner.green(), 255);
    QCOMPARE(corner.red(), 0);

    QColor center(downloaded.pixel(32, 32));
    QCOMPARE(center.red(), 255);
    QCOMPARE(center.green(), 0);
}

void VulkanTest::testUpdateCorners()
{
    QImage source(64, 64, QImage::Format_RGBA8888_Premultiplied);
    source.fill(QColor(0, 0, 0, 255));

    auto texture = VulkanTexture::upload(m_device, source,
                                         vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled);
    QVERIFY(texture != nullptr);

    QImage updateSource(64, 64, QImage::Format_RGBA8888_Premultiplied);
    updateSource.fill(QColor(0, 0, 0, 255));

    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            updateSource.setPixelColor(x, y, QColor(255, 0, 0, 255));
            updateSource.setPixelColor(56 + x, y, QColor(255, 0, 0, 255));
            updateSource.setPixelColor(x, 56 + y, QColor(255, 0, 0, 255));
            updateSource.setPixelColor(56 + x, 56 + y, QColor(255, 0, 0, 255));
        }
    }

    Region corners;
    corners += Rect(0, 0, 8, 8);
    corners += Rect(56, 0, 8, 8);
    corners += Rect(0, 56, 8, 8);
    corners += Rect(56, 56, 8, 8);
    QVERIFY(texture->update(updateSource, corners));

    QImage downloaded = texture->download();
    QVERIFY(!downloaded.isNull());

    QCOMPARE(QColor(downloaded.pixel(0, 0)).red(), 255);
    QCOMPARE(QColor(downloaded.pixel(63, 0)).red(), 255);
    QCOMPARE(QColor(downloaded.pixel(0, 63)).red(), 255);
    QCOMPARE(QColor(downloaded.pixel(63, 63)).red(), 255);

    QCOMPARE(QColor(downloaded.pixel(32, 32)).red(), 0);
}

void VulkanTest::testComputeQueue()
{
    const auto &properties = m_device->queueFamilyProperties()[m_device->computeQueueFamily()];
    QVERIFY(properties.queueFlags & VK_QUEUE_COMPUTE_BIT);
    if (m_device->hasDedicatedComputeQueue()) {
        QVERIFY(!(properties.queueFlags & VK_QUEUE_GRAPHICS_BIT));
        QVERIFY(m_device->computeQueueFamily() != m_device->graphicsQueueFamily());
    }
}

void VulkanTest::testSwapchainBufferAge()
{
    const ModifierList modifiers = m_device->computeOutputFormats().value(DRM_FORMAT_ABGR8888);
    if (modifiers.isEmpty()) {
        QSKIP("No Vulkan ABGR8888 output format available");
    }
    auto swapchain = VulkanSwapchain::create(m_device,
                                             m_renderDevice->drmDevice()->allocator(),
                                             QSize(64, 64),
                                             DRM_FORMAT_ABGR8888,
                                             modifiers,
                                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    QVERIFY(swapchain);

    auto first = swapchain->acquire();
    QVERIFY(first);
    VulkanSwapchainSlot *const firstPointer = first.get();
    QCOMPARE(first->age(), 0);

    // Importing a slot for a presentation test leaves its contents and age
    // unchanged. Once the temporary framebuffer reference is gone, acquiring
    // the slot again must still require a full repaint.
    GraphicsBufferRef testFramebuffer(first->buffer());
    first.reset();
    testFramebuffer.reset();
    first = swapchain->acquire();
    QCOMPARE(first.get(), firstPointer);
    QCOMPARE(first->age(), 0);

    swapchain->releaseRendered(first.get(), FileDescriptor{});
    QCOMPARE(first->age(), 1);

    // Hold the first rendered slot as if it were scanned out, then render a
    // second slot. Only actual renders advance the age sequence.
    GraphicsBufferRef scanout(first->buffer());
    auto second = swapchain->acquire();
    QVERIFY(second);
    QVERIFY(second.get() != first.get());
    QCOMPARE(second->age(), 0);
    swapchain->releaseRendered(second.get(), FileDescriptor{});
    QCOMPARE(second->age(), 1);
    QCOMPARE(first->age(), 2);
}

void VulkanTest::testTimestampQueryWait()
{
    if (!m_device->hasHostQueryReset()) {
        QSKIP("Vulkan device does not support host query reset");
    }

    auto compositor = VulkanCompositor::create(m_device);
    QVERIFY(compositor);
    const QList<VulkanSolidLayer> layers{
        {QRectF(0, 0, 1920, 1080), QColor(30, 90, 180, 255)},
        {QRectF(160, 120, 1600, 840), QColor(210, 60, 40, 128)},
    };
    auto result = compositor->render(QSize(1920, 1080), layers, Qt::black);
    QVERIFY(result);
    QVERIFY(result->preprocessTime);
    QVERIFY(result->compositeTime);

    // Deliberately read before waiting on the exported completion sync fd.
    // WAIT_BIT must observe the host-reset pool and wait for the timestamp
    // writes, rather than treating fresh uninitialized storage as available.
    const auto preprocess = result->preprocessTime->gpuDuration();
    const auto composite = result->compositeTime->gpuDuration();
    QVERIFY(preprocess.has_value());
    QVERIFY(composite.has_value());
    QVERIFY(*preprocess > std::chrono::nanoseconds::zero());
    QVERIFY(*composite > std::chrono::nanoseconds::zero());
}

void VulkanTest::testTimestampQueriesDisabled()
{
    auto compositor = VulkanCompositor::create(m_device);
    QVERIFY(compositor);
    auto target = VulkanTexture::allocate(m_device,
                                          vk::Format::eR8G8B8A8Unorm,
                                          QSize(64, 64),
                                          vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc,
                                          VulkanQueueRole::Compute);
    QVERIFY(target);

    const QList<VulkanCompositorLayer> layers{{
        .rect = QRectF(0, 0, 64, 64),
        .color = QColor(30, 90, 180, 255),
        .colorDescription = nullptr,
    }};
    auto result = compositor->renderTo(target.get(),
                                       layers,
                                       Qt::black,
                                       Region(0, 0, 64, 64),
                                       {},
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       VulkanCompositor::Timing::Disabled);
    QVERIFY(result);
    QVERIFY(result->completionFence.isValid());
    QVERIFY(!result->preprocessTime);
    QVERIFY(!result->compositeTime);
    m_device->waitComputeIdle();
}

void VulkanTest::testComputeSolidScenes_data()
{
    QTest::addColumn<QSize>("size");
    QTest::addColumn<QColor>("background");
    QTest::addColumn<QList<VulkanSolidLayer>>("layers");

    QTest::newRow("empty-edge-tile")
        << QSize(17, 31)
        << QColor(12, 34, 56, 255)
        << QList<VulkanSolidLayer>{};

    QTest::newRow("tile-boundaries-and-overdraw")
        << QSize(37, 29)
        << QColor(8, 16, 24, 255)
        << QList<VulkanSolidLayer>{
               {QRectF(0, 0, 16, 16), QColor(255, 0, 0, 255)},
               {QRectF(15, 7, 18, 17), QColor(0, 255, 0, 128)},
               {QRectF(29, 0, 8, 29), QColor(0, 0, 255, 192)},
               {QRectF(-4, 22, 22, 12), QColor(255, 255, 0, 96)},
           };

    QList<VulkanSolidLayer> sixtyFourLayers;
    for (int i = 0; i < 63; ++i) {
        sixtyFourLayers.append({QRectF(i % 8, i / 8, 1, 1), QColor(0, 0, 0, 0)});
    }
    sixtyFourLayers.append({QRectF(0, 0, 19, 18), QColor(201, 17, 123, 255)});
    QTest::newRow("highest-bit-and-partial-tile")
        << QSize(19, 18)
        << QColor(Qt::black)
        << sixtyFourLayers;

    QList<VulkanSolidLayer> manyLayers;
    for (int i = 0; i < 136; ++i) {
        manyLayers.append({QRectF(0, 0, 23, 21), QColor(17, 31, 47, 0)});
    }
    manyLayers.append({QRectF(0, 0, 23, 21), QColor(73, 149, 211, 255)});
    QTest::newRow("more-than-one-mask-word")
        << QSize(23, 21)
        << QColor(5, 9, 13, 255)
        << manyLayers;

    QList<VulkanSolidLayer> multiBinLayers;
    for (int i = 0; i < 33; ++i) {
        multiBinLayers.append({QRectF((i * 37) % 520, (i * 53) % 360, 19, 23), QColor(0, 0, 0, 0)});
    }
    multiBinLayers.append({QRectF(7, 11, 511, 351), QColor(41, 83, 137, 255)});
    multiBinLayers.append({QRectF(219, 143, 297, 219), QColor(227, 61, 43, 176)});
    multiBinLayers.append({QRectF(483, 5, 47, 373), QColor(29, 211, 109, 208)});
    QTest::newRow("prefix-multiple-2d-bins")
        << QSize(541, 389)
        << QColor(3, 7, 13, 255)
        << multiBinLayers;
}

void VulkanTest::testComputeSolidScenes()
{
    QFETCH(QSize, size);
    QFETCH(QColor, background);
    QFETCH(QList<VulkanSolidLayer>, layers);

    auto compositor = VulkanCompositor::create(m_device);
    QVERIFY(compositor);
    auto result = compositor->render(size, layers, background);
    QVERIFY(result);
    QVERIFY(result->completionFence.isValid());

    const QImage actual = result->texture->download();
    const QImage expected = referenceImage(size, background, layers);
    QVERIFY(!actual.isNull());
    const int difference = maximumChannelDifference(actual, expected);
    QVERIFY2(difference <= 1, qPrintable(QStringLiteral("maximum channel difference was %1").arg(difference)));

    if (result->preprocessTime) {
        QVERIFY(result->preprocessTime->query().has_value());
    }
    if (result->compositeTime) {
        QVERIFY(result->compositeTime->query().has_value());
    }
}

void VulkanTest::testComputeTexturedLayers()
{
    QImage checker(4, 4, QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < checker.height(); ++y) {
        for (int x = 0; x < checker.width(); ++x) {
            checker.setPixelColor(x, y, QColor((x & 1) ? 240 : 20, (y & 1) ? 210 : 30, ((x + y) & 1) ? 80 : 220, 64 + (x + y) * 24));
        }
    }
    QImage stripes(3, 2, QImage::Format_RGBA8888_Premultiplied);
    stripes.fill(Qt::transparent);
    stripes.setPixelColor(0, 0, QColor(255, 0, 0, 255));
    stripes.setPixelColor(1, 0, QColor(0, 255, 0, 192));
    stripes.setPixelColor(2, 0, QColor(0, 0, 255, 128));
    stripes.setPixelColor(0, 1, QColor(255, 255, 0, 96));
    stripes.setPixelColor(1, 1, QColor(0, 255, 255, 224));
    stripes.setPixelColor(2, 1, QColor(255, 0, 255, 160));

    const auto usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
    auto checkerTexture = VulkanTexture::upload(m_device, checker, usage, VulkanQueueRole::Compute);
    auto stripesTexture = VulkanTexture::upload(m_device, stripes, usage, VulkanQueueRole::Compute);
    QVERIFY(checkerTexture);
    QVERIFY(stripesTexture);

    const QList<VulkanCompositorLayer> layers{
        {.rect = QRectF(2, 1, 9, 6), .color = QColor(17, 80, 210, 180), .opacity = 0.65},
        {.rect = QRectF(1, 2, 4, 4), .texture = checkerTexture.get()},
        {.rect = QRectF(6, 0, 3, 2), .texture = stripesTexture.get(), .opacity = 0.8},
        {.rect = QRectF(12, 3, 2, 2), .texture = checkerTexture.get(), .sourceRect = QRectF(1, 1, 2, 2)},
    };
    const QSize outputSize(19, 13);
    const QColor background(11, 19, 31, 255);

    QImage expected(outputSize, QImage::Format_RGBA8888_Premultiplied);
    expected.fill(background);
    QPainter painter(&expected);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    for (const VulkanCompositorLayer &layer : layers) {
        painter.save();
        painter.setOpacity(layer.opacity);
        if (layer.texture == checkerTexture.get()) {
            const QRectF source = layer.sourceRect.isEmpty() ? QRectF(QPointF(0, 0), QSizeF(checker.size())) : layer.sourceRect;
            painter.drawImage(layer.rect, checker, source);
        } else if (layer.texture == stripesTexture.get()) {
            painter.drawImage(layer.rect, stripes, QRectF(QPointF(0, 0), QSizeF(stripes.size())));
        } else {
            painter.fillRect(layer.rect, layer.color);
        }
        painter.restore();
    }
    painter.end();

    auto compositor = VulkanCompositor::create(m_device);
    QVERIFY(compositor);
    auto result = compositor->render(outputSize, layers, background);
    QVERIFY(result);
    const QImage actual = result->texture->download();
    QVERIFY(!actual.isNull());
    const int difference = maximumChannelDifference(actual, expected);
    QVERIFY2(difference <= 2, qPrintable(QStringLiteral("maximum textured channel difference was %1").arg(difference)));
}

void VulkanTest::testComputeDescriptorBatches()
{
    constexpr int textureCount = 20;
    const QSize outputSize(61, 15);
    const QColor background(9, 17, 29, 255);
    const auto usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;

    std::vector<QImage> images;
    std::vector<std::unique_ptr<VulkanTexture>> textures;
    std::vector<VulkanCompositorLayer> layers;
    images.reserve(textureCount);
    textures.reserve(textureCount);
    layers.reserve(textureCount + 1);

    for (int i = 0; i < textureCount; ++i) {
        QImage image(1, 1, QImage::Format_RGBA8888_Premultiplied);
        image.fill(QColor((37 * i + 41) % 256,
                          (83 * i + 19) % 256,
                          (131 * i + 7) % 256,
                          255));
        images.push_back(image);
        textures.push_back(VulkanTexture::upload(m_device, images.back(), usage, VulkanQueueRole::Compute));
        QVERIFY(textures.back());

        const int column = i % 10;
        const int row = i / 10;
        layers.push_back({
            .rect = QRectF(1 + column * 6, 1 + row * 6, 5, 5),
            .texture = textures.back().get(),
        });
    }

    // This layer belongs to the second descriptor batch but modifies pixels
    // produced by the first one. It verifies both destination preservation and
    // destination-out blending across the batch boundary.
    layers.push_back({
        .rect = QRectF(1, 1, 5, 5),
        .color = Qt::white,
        .blendMode = VulkanBlendMode::DestinationOut,
    });

    QImage expected(outputSize, QImage::Format_RGBA8888_Premultiplied);
    expected.fill(background);
    QPainter painter(&expected);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    for (int i = 0; i < textureCount; ++i) {
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.drawImage(layers[i].rect, images[i]);
    }
    painter.setCompositionMode(QPainter::CompositionMode_DestinationOut);
    painter.fillRect(layers.back().rect, Qt::white);
    painter.end();

    auto compositor = VulkanCompositor::create(m_device);
    QVERIFY(compositor);
    auto result = compositor->render(outputSize, layers, background);
    QVERIFY(result);
    const QImage actual = result->texture->download();
    QVERIFY(!actual.isNull());
    const int difference = maximumChannelDifference(actual, expected);
    QVERIFY2(difference <= 2, qPrintable(QStringLiteral("maximum descriptor-batch channel difference was %1").arg(difference)));
}

void VulkanTest::testComputeQuadGeometry()
{
    const QColor background(7, 11, 19, 255);
    const QColor foreground(35, 210, 95, 255);
    QTransform transform;
    transform.translate(1, 0);
    const QList<VulkanCompositorLayer> layers{
        {
            .rect = QRectF(0, 0, 20, 16),
            .color = foreground,
            .transform = transform,
            .quadVertices = std::array{
                QPointF(2, 2),
                QPointF(17, 3),
                QPointF(14, 14),
                QPointF(4, 12),
            },
        },
    };

    auto compositor = VulkanCompositor::create(m_device);
    QVERIFY(compositor);
    auto result = compositor->render(QSize(21, 17), layers, background);
    QVERIFY(result);
    const QImage actual = result->texture->download();
    QVERIFY(!actual.isNull());

    QCOMPARE(actual.pixelColor(10, 7), foreground);
    QCOMPARE(actual.pixelColor(2, 13), background);
    QCOMPARE(actual.pixelColor(18, 13), background);
    QCOMPARE(actual.pixelColor(1, 1), background);
}

void VulkanTest::testComputeTransformsAndClipping()
{
    QImage source(3, 2, QImage::Format_RGBA8888_Premultiplied);
    source.setPixelColor(0, 0, Qt::red);
    source.setPixelColor(1, 0, Qt::green);
    source.setPixelColor(2, 0, Qt::blue);
    source.setPixelColor(0, 1, Qt::yellow);
    source.setPixelColor(1, 1, Qt::cyan);
    source.setPixelColor(2, 1, Qt::magenta);
    auto texture = VulkanTexture::upload(m_device,
                                         source,
                                         vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
                                         VulkanQueueRole::Compute);
    QVERIFY(texture);

    QTransform scaled;
    scaled.translate(3, 2);
    scaled.scale(1.5, 1.25);
    QTransform rotated;
    rotated.translate(17, 1);
    rotated.rotate(90);
    QTransform translated;
    translated.translate(5, 8);
    const QTransform flipTexture(-1, 0, 0, 1, 1, 0);

    const QList<VulkanCompositorLayer> layers{
        {
            .rect = QRectF(0, 0, 4, 4),
            .color = QColor(210, 40, 30, 255),
            .transform = scaled,
            .clipRect = QRectF(7, 4, 5, 5),
        },
        {
            .rect = QRectF(0, 0, 4, 6),
            .color = QColor(20, 190, 80, 255),
            .transform = rotated,
        },
        {
            .rect = QRectF(0, 0, 6, 4),
            .texture = texture.get(),
            .transform = translated,
            .textureTransform = flipTexture,
        },
    };
    const QSize outputSize(21, 15);
    const QColor background(9, 13, 21, 255);

    QImage expected(outputSize, QImage::Format_RGBA8888_Premultiplied);
    expected.fill(background);
    QPainter painter(&expected);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    for (const VulkanCompositorLayer &layer : layers) {
        painter.save();
        if (layer.clipRect) {
            painter.setClipRect(*layer.clipRect, Qt::IntersectClip);
        }
        painter.setTransform(layer.transform, true);
        if (layer.texture) {
            painter.drawImage(layer.rect, source.mirrored(true, false));
        } else {
            painter.fillRect(layer.rect, layer.color);
        }
        painter.restore();
    }
    painter.end();

    auto compositor = VulkanCompositor::create(m_device);
    QVERIFY(compositor);
    auto result = compositor->render(outputSize, layers, background);
    QVERIFY(result);
    const QImage actual = result->texture->download();
    QVERIFY(!actual.isNull());
    const int difference = maximumChannelDifference(actual, expected);
    QVERIFY2(difference <= 2, qPrintable(QStringLiteral("maximum transformed channel difference was %1").arg(difference)));
}

void VulkanTest::testComputeDamageTiles()
{
    auto compositor = VulkanCompositor::create(m_device);
    QVERIFY(compositor);
    const QSize size(35, 27);
    const QColor background(7, 11, 19, 255);
    const QList<VulkanSolidLayer> baseLayers{
        {QRectF(0, 0, 35, 27), QColor(80, 30, 170, 255)},
    };
    auto first = compositor->render(size, baseLayers, background);
    QVERIFY(first);
    const QImage baseImage = first->texture->download();
    QCOMPARE(maximumChannelDifference(baseImage, referenceImage(size, background, baseLayers)), 0);

    const QList<VulkanSolidLayer> changedLayers{
        baseLayers.front(),
        {QRectF(18, 3, 5, 7), QColor(30, 230, 90, 192)},
    };
    const Region damage(18, 3, 5, 7);
    auto changed = compositor->render(size, changedLayers, background, damage);
    QVERIFY(changed);
    const QImage changedImage = changed->texture->download();
    const QImage changedReference = referenceImage(size, background, changedLayers);
    QVERIFY(maximumChannelDifference(changedImage, changedReference) <= 1);

    const QList<VulkanSolidLayer> ignoredLayers{
        {QRectF(0, 0, 35, 27), QColor(255, 255, 255, 255)},
    };
    auto noDamage = compositor->render(size, ignoredLayers, background, Region{});
    QVERIFY(noDamage);
    const QImage unchangedImage = noDamage->texture->download();
    QCOMPARE(maximumChannelDifference(unchangedImage, changedImage), 0);
}

void VulkanTest::testComputeLargeDispatch()
{
    // One workgroup is dispatched per dirty tile. This is just over Vulkan's
    // guaranteed 65,535 workgroups in one dimension and therefore exercises
    // the compositor's two-dimensional dispatch indexing.
    const QSize size(4096, 4112);
    const QColor color(37, 149, 211, 255);
    const std::array layers{VulkanSolidLayer{
        .rect = QRectF(QPointF(), QSizeF(size)),
        .color = color,
    }};
    auto compositor = VulkanCompositor::create(m_device);
    QVERIFY(compositor);
    auto result = compositor->render(size, layers);
    QVERIFY(result);
    const QImage image = result->texture->download();
    QVERIFY(!image.isNull());
    QCOMPARE(image.pixelColor(0, 0), color);
    QCOMPARE(image.pixelColor(size.width() - 1, size.height() - 1), color);
}

void VulkanTest::testComputeBrightnessAndSaturation()
{
    QImage source(2, 1, QImage::Format_RGBA8888_Premultiplied);
    source.setPixelColor(0, 0, Qt::red);
    source.setPixelColor(1, 0, Qt::green);
    auto texture = VulkanTexture::upload(m_device,
                                         source,
                                         vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
                                         VulkanQueueRole::Compute);
    QVERIFY(texture);

    const QList<VulkanCompositorLayer> layers{
        {
            .rect = QRectF(0, 0, 2, 1),
            .texture = texture.get(),
            .brightness = 0.5,
            .saturation = 0.0,
        },
    };
    auto compositor = VulkanCompositor::create(m_device);
    QVERIFY(compositor);
    auto result = compositor->render(QSize(2, 1), layers, Qt::black);
    QVERIFY(result);
    const QImage actual = result->texture->download();
    QVERIFY(!actual.isNull());

    const QColor redPixel = actual.pixelColor(0, 0);
    QVERIFY(std::abs(redPixel.red() - 27) <= 1);
    QVERIFY(std::abs(redPixel.green() - 27) <= 1);
    QVERIFY(std::abs(redPixel.blue() - 27) <= 1);
    QCOMPARE(redPixel.alpha(), 255);

    const QColor greenPixel = actual.pixelColor(1, 0);
    QVERIFY(std::abs(greenPixel.red() - 91) <= 1);
    QVERIFY(std::abs(greenPixel.green() - 91) <= 1);
    QVERIFY(std::abs(greenPixel.blue() - 91) <= 1);
    QCOMPARE(greenPixel.alpha(), 255);
}

void VulkanTest::testComputeColorConversion()
{
    const auto sourceColor = std::make_shared<ColorDescription>(Colorimetry::BT709, TransferFunction(TransferFunction::sRGB));
    const auto targetColor = std::make_shared<ColorDescription>(Colorimetry::DisplayP3, TransferFunction(TransferFunction::gamma22));
    const QColor encodedSource(180, 92, 37);
    const QList<VulkanCompositorLayer> layers{
        {
            .rect = QRectF(0, 0, 4, 4),
            .color = encodedSource,
            .colorDescription = sourceColor,
            .renderingIntent = RenderingIntent::RelativeColorimetric,
        },
    };

    auto compositor = VulkanCompositor::create(m_device);
    QVERIFY(compositor);
    auto result = compositor->render(QSize(4, 4), layers, Qt::transparent, Region::infinite(), targetColor);
    QVERIFY(result);
    const QColor actual = result->texture->download().pixelColor(1, 1);

    const QVector3D source(encodedSource.redF(), encodedSource.greenF(), encodedSource.blueF());
    const QVector3D mapped = sourceColor->mapTo(source, *targetColor, RenderingIntent::RelativeColorimetric);
    const QColor expected = QColor::fromRgbF(std::clamp(mapped.x(), 0.0f, 1.0f),
                                             std::clamp(mapped.y(), 0.0f, 1.0f),
                                             std::clamp(mapped.z(), 0.0f, 1.0f));
    QVERIFY(std::abs(actual.red() - expected.red()) <= 2);
    QVERIFY(std::abs(actual.green() - expected.green()) <= 2);
    QVERIFY(std::abs(actual.blue() - expected.blue()) <= 2);
    QCOMPARE(actual.alpha(), 255);

    auto identityResult = compositor->render(QSize(4, 4), layers, Qt::transparent, Region::infinite(), sourceColor);
    QVERIFY(identityResult);
    const QColor identityActual = identityResult->texture->download().pixelColor(1, 1);
    QVERIFY(std::abs(identityActual.red() - encodedSource.red()) <= 1);
    QVERIFY(std::abs(identityActual.green() - encodedSource.green()) <= 1);
    QVERIFY(std::abs(identityActual.blue() - encodedSource.blue()) <= 1);
    QCOMPARE(identityActual.alpha(), encodedSource.alpha());
}

void VulkanTest::testComputeHdrToneMapping()
{
    const auto sourceColor = std::make_shared<ColorDescription>(Colorimetry::BT2020,
                                                                TransferFunction(TransferFunction::PerceptualQuantizer, 0, 10'000),
                                                                203,
                                                                0,
                                                                1'000,
                                                                1'000);
    const auto targetColor = std::make_shared<ColorDescription>(Colorimetry::BT2020,
                                                                TransferFunction(TransferFunction::gamma22, 0, 300),
                                                                203,
                                                                0,
                                                                300,
                                                                300);
    const float encoded = sourceColor->transferFunction().nitsToEncoded(800);
    const QColor encodedSource = QColor::fromRgbF(encoded, encoded, encoded);
    const QList<VulkanCompositorLayer> layers{
        {
            .rect = QRectF(0, 0, 4, 4),
            .color = encodedSource,
            .colorDescription = sourceColor,
            .renderingIntent = RenderingIntent::Perceptual,
        },
    };

    auto compositor = VulkanCompositor::create(m_device);
    QVERIFY(compositor);
    auto result = compositor->render(QSize(4, 4), layers, Qt::transparent, Region::infinite(), targetColor);
    QVERIFY(result);
    const QColor actual = result->texture->download().pixelColor(1, 1);

    const QVector3D input(encodedSource.redF(), encodedSource.greenF(), encodedSource.blueF());
    const QVector3D mapped = ColorPipeline::create(sourceColor, targetColor, RenderingIntent::Perceptual).evaluate(input);
    const QColor expected = QColor::fromRgbF(std::clamp(mapped.x(), 0.0f, 1.0f),
                                             std::clamp(mapped.y(), 0.0f, 1.0f),
                                             std::clamp(mapped.z(), 0.0f, 1.0f));
    QVERIFY(std::abs(actual.red() - expected.red()) <= 3);
    QVERIFY(std::abs(actual.green() - expected.green()) <= 3);
    QVERIFY(std::abs(actual.blue() - expected.blue()) <= 3);
    QCOMPARE(actual.alpha(), 255);
}

void VulkanTest::testComputeOutputColorPipeline()
{
    ColorPipeline pipeline(ValueRange{0, 1}, ColorspaceType::NonLinearRGB);
    pipeline.addTransferFunction(TransferFunction(TransferFunction::sRGB, 0, 1), ColorspaceType::LinearRGB);
    const QMatrix4x4 calibration(0.82f, 0.13f, 0.05f, 0.0f,
                                 0.04f, 0.91f, 0.05f, 0.0f,
                                 0.03f, 0.09f, 0.88f, 0.0f,
                                 0.0f, 0.0f, 0.0f, 1.0f);
    pipeline.addMatrix(calibration, ValueRange{0, 1}, ColorspaceType::LinearRGB);
    pipeline.addInverseTransferFunction(TransferFunction(TransferFunction::gamma22, 0, 1), ColorspaceType::NonLinearRGB);

    const QSize size(9, 7);
    const QColor input(181, 93, 37);
    const QVector3D output = pipeline.evaluate(QVector3D(input.redF(), input.greenF(), input.blueF()));
    const QColor expected = QColor::fromRgbF(std::clamp(output.x(), 0.0f, 1.0f),
                                             std::clamp(output.y(), 0.0f, 1.0f),
                                             std::clamp(output.z(), 0.0f, 1.0f));

    auto texture = VulkanTexture::allocate(m_device,
                                           vk::Format::eR8G8B8A8Unorm,
                                           size,
                                           vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
                                           VulkanQueueRole::Compute);
    QVERIFY(texture);
    VulkanRenderTarget vulkanTarget(texture.get(), FileDescriptor{}, OutputTransform::Kind::Normal, &pipeline);
    const RenderTarget target(&vulkanTarget);
    const RenderViewport viewport(RectF(QPointF(), QSizeF(size)), 1.0, target, QPoint());
    auto renderer = std::make_unique<ItemRendererVulkan>(m_device);
    QVERIFY(renderer->isValid());
    renderer->beginFrame(target, viewport);
    renderer->renderBackground(target, viewport, Region(Rect(QPoint(), size)));
    QPainter *painter = renderer->painter();
    QVERIFY(painter);
    painter->fillRect(QRectF(QPointF(), QSizeF(size)), input);
    renderer->endFrame();

    const QColor actual = texture->download().pixelColor(4, 3);
    QVERIFY(std::abs(actual.red() - expected.red()) <= 2);
    QVERIFY(std::abs(actual.green() - expected.green()) <= 2);
    QVERIFY(std::abs(actual.blue() - expected.blue()) <= 2);
    QCOMPARE(actual.alpha(), 255);
}

void VulkanTest::testComputeColorFilters()
{
    QMatrix4x4 swapRedBlue;
    swapRedBlue.setToIdentity();
    swapRedBlue(0, 0) = 0;
    swapRedBlue(0, 2) = 1;
    swapRedBlue(2, 0) = 1;
    swapRedBlue(2, 2) = 0;
    const QColor input(41, 103, 219);
    const QColor flash(227, 36, 84);
    const QList<VulkanCompositorLayer> layers{
        {
            .rect = QRectF(0, 0, 4, 4),
            .color = input,
            .colorFilter = VulkanColorFilter::Invert,
            .colorDescription = ColorDescription::sRGB,
        },
        {
            .rect = QRectF(4, 0, 4, 4),
            .color = input,
            .colorFilter = VulkanColorFilter::Colorize,
            .colorFilterParameters = QVector4D(flash.redF(), flash.greenF(), flash.blueF(), flash.alphaF()),
            .colorDescription = ColorDescription::sRGB,
        },
        {
            .rect = QRectF(8, 0, 4, 4),
            .color = input,
            .colorFilter = VulkanColorFilter::ColorBlindnessCorrection,
            .colorFilterMatrix = swapRedBlue,
            .colorDescription = ColorDescription::sRGB,
        },
    };
    auto compositor = VulkanCompositor::create(m_device);
    QVERIFY(compositor);
    auto result = compositor->render(QSize(12, 4), layers, Qt::transparent, Region::infinite(), ColorDescription::sRGB);
    QVERIFY(result);
    const QImage actual = result->texture->download();
    const QColor inverted = actual.pixelColor(1, 1);
    const TransferFunction destinationTransfer = ColorDescription::sRGB->transferFunction();
    const TransferFunction inversionTransfer(TransferFunction::gamma22, 0, ColorDescription::sRGB->referenceLuminance());
    const auto invertChannel = [&destinationTransfer, &inversionTransfer](qreal encoded) {
        const double nits = destinationTransfer.encodedToNits(encoded);
        const double invertedGamma = 1.0 - inversionTransfer.nitsToEncoded(nits);
        return destinationTransfer.nitsToEncoded(inversionTransfer.encodedToNits(invertedGamma));
    };
    const QColor expectedInverted = QColor::fromRgbF(invertChannel(input.redF()),
                                                     invertChannel(input.greenF()),
                                                     invertChannel(input.blueF()));
    QVERIFY(std::abs(inverted.red() - expectedInverted.red()) <= 1);
    QVERIFY(std::abs(inverted.green() - expectedInverted.green()) <= 1);
    QVERIFY(std::abs(inverted.blue() - expectedInverted.blue()) <= 1);
    QCOMPARE(actual.pixelColor(5, 1), flash);
    const QColor corrected = actual.pixelColor(9, 1);
    QVERIFY(std::abs(corrected.red() - input.blue()) <= 1);
    QVERIFY(std::abs(corrected.green() - input.green()) <= 1);
    QVERIFY(std::abs(corrected.blue() - input.red()) <= 1);
}

void VulkanTest::testComputeFractionalDebug()
{
    QImage source(QSize(4, 4), QImage::Format_RGBA8888_Premultiplied);
    source.fill(Qt::white);
    auto texture = VulkanTexture::upload(m_device,
                                         source,
                                         vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
                                         VulkanQueueRole::Compute);
    QVERIFY(texture);

    const QColor background(7, 11, 13, 255);
    auto compositor = VulkanCompositor::create(m_device);
    QVERIFY(compositor);

    VulkanCompositorLayer debugLayer{
        .rect = QRectF(0, 0, 4, 4),
        .texture = texture.get(),
        .colorFilter = VulkanColorFilter::FractionalDebug,
    };
    auto aligned = compositor->render(QSize(6, 6), std::span(&debugLayer, 1), background);
    QVERIFY(aligned);
    QCOMPARE(aligned->texture->download().pixelColor(1, 1), background);

    debugLayer.transform.translate(0.25, 0.25);
    auto fractional = compositor->render(QSize(6, 6), std::span(&debugLayer, 1), background);
    QVERIFY(fractional);
    const QColor marked = fractional->texture->download().pixelColor(1, 1);
    QVERIFY(marked.red() > 80); // Fractional texture lookup: red.
    QVERIFY(marked.blue() > 40); // Fractional transformed vertices: blue.
    QCOMPARE(marked.alpha(), 255);
}

void VulkanTest::testComputeZoomFilters()
{
    auto compositor = VulkanCompositor::create(m_device);
    QVERIFY(compositor);
    const auto usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;

    QImage whiteSource(QSize(2, 2), QImage::Format_RGBA8888_Premultiplied);
    whiteSource.fill(Qt::white);
    auto whiteTexture = VulkanTexture::upload(m_device, whiteSource, usage, VulkanQueueRole::Compute);
    QVERIFY(whiteTexture);
    const VulkanCompositorLayer gridLayer{
        .rect = QRectF(0, 0, 16, 16),
        .texture = whiteTexture.get(),
        .colorFilter = VulkanColorFilter::ZoomPixelGrid,
        .colorDescription = ColorDescription::sRGB,
    };
    auto gridResult = compositor->render(QSize(16, 16), std::span(&gridLayer, 1), Qt::transparent, Region::infinite(), ColorDescription::sRGB);
    QVERIFY(gridResult);
    const QImage gridImage = gridResult->texture->download();
    QCOMPARE(gridImage.pixelColor(3, 3), QColor(Qt::white));
    QVERIFY(gridImage.pixelColor(7, 3).red() < 200);
    QCOMPARE(gridImage.pixelColor(7, 3).alpha(), 255);

    QImage pattern(QSize(5, 5), QImage::Format_RGBA8888_Premultiplied);
    pattern.fill(Qt::white);
    for (int y = 0; y < pattern.height(); ++y) {
        pattern.setPixelColor(std::min(y, pattern.width() - 1), y, Qt::black);
        if (y + 1 < pattern.width()) {
            pattern.setPixelColor(y + 1, y, Qt::black);
        }
    }
    auto patternTexture = VulkanTexture::upload(m_device, pattern, usage, VulkanQueueRole::Compute);
    QVERIFY(patternTexture);
    VulkanCompositorLayer patternLayer{
        .rect = QRectF(0, 0, 40, 40),
        .texture = patternTexture.get(),
        .colorFilter = VulkanColorFilter::ZoomPatternUpscale,
        .colorFilterParameters = QVector4D(8.0, 0.0, 0.0, 0.0),
        .colorDescription = ColorDescription::sRGB,
    };
    auto patternResult = compositor->render(QSize(40, 40), std::span(&patternLayer, 1), Qt::transparent, Region::infinite(), ColorDescription::sRGB);
    QVERIFY(patternResult);
    const QImage patternImage = patternResult->texture->download();
    patternLayer.colorFilter = VulkanColorFilter::None;
    auto linearResult = compositor->render(QSize(40, 40), std::span(&patternLayer, 1), Qt::transparent, Region::infinite(), ColorDescription::sRGB);
    QVERIFY(linearResult);
    const QImage linearImage = linearResult->texture->download();
    QVERIFY(maximumChannelDifference(patternImage, linearImage) > 5);
    QCOMPARE(patternImage.pixelColor(20, 20).alpha(), 255);
}

void VulkanTest::testComputeScreenTransformCrossFade()
{
    const auto uploadColor = [this](const QColor &color) {
        QImage image(QSize(1, 1), QImage::Format_RGBA8888_Premultiplied);
        image.fill(color);
        return VulkanTexture::upload(m_device,
                                     image,
                                     vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
                                     VulkanQueueRole::Compute);
    };
    auto previous = uploadColor(Qt::red);
    auto current = uploadColor(Qt::blue);
    QVERIFY(previous);
    QVERIFY(current);
    const VulkanCompositorLayer layer{
        .rect = QRectF(0, 0, 8, 8),
        .texture = current.get(),
        .auxiliaryTexture = previous.get(),
        .colorFilter = VulkanColorFilter::ScreenTransformCrossFade,
        .colorFilterParameters = QVector4D(0.25, 0.0, 0.0, 0.0),
        .colorDescription = ColorDescription::sRGB,
    };
    auto compositor = VulkanCompositor::create(m_device);
    QVERIFY(compositor);
    auto result = compositor->render(QSize(8, 8), std::span(&layer, 1), Qt::transparent, Region::infinite(), ColorDescription::sRGB);
    QVERIFY(result);
    const QColor actual = result->texture->download().pixelColor(4, 4);
    QVERIFY(std::abs(actual.red() - 191) <= 1);
    QCOMPARE(actual.green(), 0);
    QVERIFY(std::abs(actual.blue() - 64) <= 1);
    QCOMPARE(actual.alpha(), 255);
}

void VulkanTest::testComputeIccOutput_data()
{
    QTest::addColumn<QString>("profilePath");
    QTest::newRow("BToA") << QFINDTESTDATA("../data/Framework 13.icc");
    QTest::newRow("shaper-matrix") << QFINDTESTDATA("../data/Samsung CRG49 Shaper Matrix.icc");
    QTest::newRow("MHC2") << QFINDTESTDATA("../data/HP 'sRGB' profile with MHC2.icc");
}

void VulkanTest::testComputeIccOutput()
{
    QFETCH(QString, profilePath);
    const std::shared_ptr<IccProfile> profile = IccProfile::load(profilePath).value_or(nullptr);
    QVERIFY(profile);
    const ColorPipeline pipeline = ColorPipeline::createIcc(profile,
                                                            ColorDescription::sRGB,
                                                            Colorimetry::BT709,
                                                            TransferFunction::gamma22,
                                                            RenderingIntent::AbsoluteColorimetricNoAdaptation);

    const QSize size(65, 65);
    QImage input(size, QImage::Format_RGBA8888_Premultiplied);
    QImage expected(size, QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            const QColor source = QColor::fromRgbF(x / 64.0, y / 64.0, (x + y) / 128.0);
            input.setPixelColor(x, y, source);
            const QVector3D output = pipeline.evaluate(QVector3D(source.redF(), source.greenF(), source.blueF()));
            expected.setPixelColor(x, y, QColor::fromRgbF(std::clamp(output.x(), 0.0f, 1.0f), std::clamp(output.y(), 0.0f, 1.0f), std::clamp(output.z(), 0.0f, 1.0f)));
        }
    }
    auto texture = VulkanTexture::upload(m_device,
                                         input,
                                         vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
                                         VulkanQueueRole::Compute);
    QVERIFY(texture);
    const QList<VulkanCompositorLayer> layers{{
        .rect = QRectF(QPointF(), QSizeF(size)),
        .texture = texture.get(),
    }};
    auto compositor = VulkanCompositor::create(m_device);
    QVERIFY(compositor);
    auto result = compositor->render(size, layers, Qt::transparent, Region::infinite(), nullptr, &pipeline);
    QVERIFY(result);
    const QImage actual = result->texture->download();
    QCOMPARE(actual.size(), expected.size());
    QVERIFY(maximumChannelDifference(actual, expected) <= 5);
}

void VulkanTest::testComputePlanarYuv()
{
    const auto makePlane = [this](uint8_t value) {
        QImage image(4, 4, QImage::Format_Grayscale8);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                image.setPixelColor(x, y, QColor(value, value, value));
            }
        }
        return VulkanTexture::upload(m_device,
                                     image,
                                     vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
                                     VulkanQueueRole::Compute);
    };
    auto yPlane = makePlane(128);
    auto uPlane = makePlane(96);
    auto vPlane = makePlane(160);
    QVERIFY(yPlane);
    QVERIFY(uPlane);
    QVERIFY(vPlane);

    const auto yuvColor = std::make_shared<ColorDescription>(Colorimetry::BT709,
                                                             TransferFunction(TransferFunction::gamma22),
                                                             YUVMatrixCoefficients::BT709,
                                                             EncodingRange::Full);
    const QList<VulkanCompositorLayer> layers{
        {
            .rect = QRectF(0, 0, 4, 4),
            .texture = yPlane.get(),
            .texturePlanes = {yPlane.get(), uPlane.get(), vPlane.get()},
            .texturePlaneCount = 3,
            .colorDescription = yuvColor,
        },
    };
    auto compositor = VulkanCompositor::create(m_device);
    QVERIFY(compositor);
    auto result = compositor->render(QSize(4, 4), layers);
    QVERIFY(result);
    const QColor actual = result->texture->download().pixelColor(1, 1);

    const QVector4D encoded(128.0f / 255.0f, 96.0f / 255.0f, 160.0f / 255.0f, 1.0f);
    const QVector4D rgb = yuvColor->yuvMatrix() * encoded;
    const QColor expected = QColor::fromRgbF(std::clamp(rgb.x(), 0.0f, 1.0f),
                                             std::clamp(rgb.y(), 0.0f, 1.0f),
                                             std::clamp(rgb.z(), 0.0f, 1.0f));
    QVERIFY(std::abs(actual.red() - expected.red()) <= 2);
    QVERIFY(std::abs(actual.green() - expected.green()) <= 2);
    QVERIFY(std::abs(actual.blue() - expected.blue()) <= 2);
    QCOMPARE(actual.alpha(), 255);
}

void VulkanTest::testComputeRoundedGeometry()
{
    const QSize size(60, 28);
    const QColor fillColor(35, 130, 235, 220);
    const QColor outlineColor(230, 65, 35, 190);
    const QRectF fillRect(2, 3, 20, 18);
    const QRectF outerRect(28, 3, 28, 22);
    const qreal thickness = 3;
    const QRectF innerRect = outerRect.adjusted(thickness, thickness, -thickness, -thickness);
    const QList<VulkanCompositorLayer> layers{
        {
            .rect = fillRect,
            .color = fillColor,
            .roundedRect = fillRect,
            .cornerRadii = QVector4D(5, 5, 5, 5),
        },
        {
            .rect = outerRect,
            .color = outlineColor,
            .roundedRect = innerRect,
            .cornerRadii = QVector4D(5, 5, 5, 5),
            .outlineThickness = thickness,
        },
    };

    QImage expected(size, QImage::Format_RGBA8888_Premultiplied);
    expected.fill(Qt::transparent);
    QPainter painter(&expected);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    QPainterPath fillPath;
    fillPath.addRoundedRect(fillRect, 5, 5);
    painter.fillPath(fillPath, fillColor);
    QPainterPath outlinePath;
    outlinePath.setFillRule(Qt::OddEvenFill);
    outlinePath.addRoundedRect(outerRect, 8, 8);
    outlinePath.addRoundedRect(innerRect, 5, 5);
    painter.fillPath(outlinePath, outlineColor);
    painter.end();

    auto compositor = VulkanCompositor::create(m_device);
    QVERIFY(compositor);
    auto result = compositor->render(size, layers);
    QVERIFY(result);
    const QImage actual = result->texture->download();
    QVERIFY(!actual.isNull());
    const int difference = maximumChannelDifference(actual, expected);
    QVERIFY2(difference <= 40, qPrintable(QStringLiteral("maximum rounded geometry channel difference was %1").arg(difference)));

    // Exercise the scene-side outlined-border translation as well. The item
    // renderer should produce the same outline parameters as the low-level
    // compositor path above.
    TestScene scene;
    OutlinedBorderItem borderItem(QRectF(31, 6, 22, 16),
                                  BorderOutline(thickness, outlineColor, BorderRadius(5)),
                                  scene.root());
    auto renderer = std::make_unique<ItemRendererVulkan>(m_device);
    QVERIFY(renderer->isValid());
    QImage sceneImage(size, QImage::Format_RGBA8888_Premultiplied);
    sceneImage.fill(Qt::transparent);
    const RenderTarget target(&sceneImage);
    const RenderViewport viewport(RectF(QPointF(), QSizeF(size)), 1.0, target, QPoint());
    ItemRenderer *rendererPointer = renderer.get();
    scene.attachRenderer(std::move(renderer));
    rendererPointer->beginFrame(target, viewport);
    rendererPointer->renderBackground(target, viewport, Region(Rect(QPoint(), size)));
    rendererPointer->renderItem(target, viewport, scene.root(), 0, Region(Rect(QPoint(), size)), WindowPaintData(), {}, {});
    rendererPointer->endFrame();
    QVERIFY(maximumChannelDifference(sceneImage.copy(28, 3, 28, 22), actual.copy(28, 3, 28, 22)) <= 1);
}

void VulkanTest::testComputeDestinationOut()
{
    const QList<VulkanCompositorLayer> layers{
        {
            .rect = QRectF(0, 0, 16, 12),
            .color = QColor(40, 110, 220, 255),
        },
        {
            .rect = QRectF(4, 3, 8, 6),
            .color = Qt::white,
            .blendMode = VulkanBlendMode::DestinationOut,
        },
    };
    auto compositor = VulkanCompositor::create(m_device);
    QVERIFY(compositor);
    auto result = compositor->render(QSize(16, 12), layers);
    QVERIFY(result);
    const QImage image = result->texture->download();
    QCOMPARE(image.pixelColor(1, 1), QColor(40, 110, 220, 255));
    QCOMPARE(image.pixelColor(8, 6), QColor(0, 0, 0, 0));
}

void VulkanTest::testDecorationAtlasNullSprites()
{
    const QColor titleBarColor(38, 119, 207, 224);
    QImage titleBar(QSize(37, 11), QImage::Format_ARGB32_Premultiplied);
    titleBar.fill(titleBarColor);

    auto renderer = std::make_unique<ItemRendererVulkan>(m_device);
    QVERIFY(renderer->isValid());
    auto atlas = renderer->createAtlas({QImage{}, titleBar, QImage{}, QImage{}});
    QVERIFY(atlas);
    QVERIFY(atlas->sprite(0).geometry.isEmpty());
    QCOMPARE(atlas->sprite(1).geometry, titleBar.rect());
    QVERIFY(atlas->sprite(2).geometry.isEmpty());
    QVERIFY(atlas->sprite(3).geometry.isEmpty());
}

void VulkanTest::testNinePatchUpload()
{
    const auto patch = [](const QSize &size, const QColor &color) {
        QImage image(size, QImage::Format_RGBA8888_Premultiplied);
        image.fill(color);
        return image;
    };
    const QImage topLeft = patch(QSize(2, 3), Qt::red);
    const QImage top = patch(QSize(4, 1), Qt::green);
    const QImage topRight = patch(QSize(3, 2), Qt::blue);
    const QImage right = patch(QSize(1, 4), Qt::yellow);
    const QImage bottomRight = patch(QSize(2, 3), Qt::cyan);
    const QImage bottom = patch(QSize(3, 2), Qt::magenta);
    const QImage bottomLeft = patch(QSize(1, 2), Qt::white);
    const QImage left = patch(QSize(2, 3), Qt::gray);

    auto renderer = std::make_unique<ItemRendererVulkan>(m_device);
    QVERIFY(renderer->isValid());
    auto genericPatch = renderer->createNinePatch(topLeft,
                                                  top,
                                                  topRight,
                                                  right,
                                                  bottomRight,
                                                  bottom,
                                                  bottomLeft,
                                                  left);
    auto vulkanPatch = static_cast<NinePatchVulkan *>(genericPatch.get());
    QVERIFY(vulkanPatch);
    QVERIFY(vulkanPatch->texture());
    const QImage uploaded = vulkanPatch->texture()->nativeTexture()->download();
    QCOMPARE(uploaded.size(), QSize(9, 10));
    QCOMPARE(uploaded.pixelColor(0, 0), QColor(Qt::red));
    QCOMPARE(uploaded.pixelColor(3, 0), QColor(Qt::green));
    QCOMPARE(uploaded.pixelColor(8, 0), QColor(Qt::blue));
    QCOMPARE(uploaded.pixelColor(0, 4), QColor(Qt::gray));
    QCOMPARE(uploaded.pixelColor(8, 4), QColor(Qt::yellow));
    QCOMPARE(uploaded.pixelColor(0, 9), QColor(Qt::white));
    QCOMPARE(uploaded.pixelColor(4, 9), QColor(Qt::magenta));
    QCOMPARE(uploaded.pixelColor(8, 9), QColor(Qt::cyan));
}

void VulkanTest::testHighPrecisionIntermediate()
{
    const QSize size(19, 13);
    auto intermediate = VulkanTexture::allocate(m_device,
                                                vk::Format::eR16G16B16A16Sfloat,
                                                size,
                                                vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                                                VulkanQueueRole::Compute);
    if (!intermediate) {
        QSKIP("RGBA16F storage intermediates are unavailable");
    }
    auto compositor = VulkanCompositor::create(m_device);
    QVERIFY(compositor);
    const QColor source = QColor::fromRgbF(0.1234, 0.4567, 0.7891, 1.0);
    const QList<VulkanCompositorLayer> firstPass{
        {.rect = QRectF(QPointF(), QSizeF(size)), .color = source},
    };
    auto first = compositor->renderTo(intermediate.get(), firstPass, Qt::transparent, Region(0, 0, size.width(), size.height()));
    QVERIFY(first);

    const QList<VulkanCompositorLayer> secondPass{
        {.rect = QRectF(QPointF(), QSizeF(size)), .texture = intermediate.get()},
    };
    auto second = compositor->render(size, secondPass, Qt::transparent, Region(0, 0, size.width(), size.height()));
    QVERIFY(second);
    const QImage result = second->texture->download();
    QVERIFY(!result.isNull());
    const QColor actual = result.pixelColor(size.width() / 2, size.height() / 2);
    QVERIFY(std::abs(actual.redF() - source.redF()) <= 1.5 / 255.0);
    QVERIFY(std::abs(actual.greenF() - source.greenF()) <= 1.5 / 255.0);
    QVERIFY(std::abs(actual.blueF() - source.blueF()) <= 1.5 / 255.0);
}

void VulkanTest::testNativeRenderTarget()
{
    const QSize size(35, 27);
    auto target = VulkanTexture::allocate(m_device,
                                          vk::Format::eR8G8B8A8Unorm,
                                          size,
                                          vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
                                          VulkanQueueRole::Compute);
    QVERIFY(target);
    auto compositor = VulkanCompositor::create(m_device);
    QVERIFY(compositor);

    const QColor background(13, 17, 29, 255);
    const QList<VulkanCompositorLayer> initialLayers{
        {.rect = QRectF(1, 2, 31, 22), .color = QColor(120, 40, 190, 255)},
    };
    auto initial = compositor->renderTo(target.get(), initialLayers, background, Region(Rect(QPoint(), size)));
    QVERIFY(initial);
    QCOMPARE(initial->texture, target.get());

    auto acquireCommand = m_device->createComputeCommandBuffer();
    QCOMPARE(acquireCommand.begin(vk::CommandBufferBeginInfo{vk::CommandBufferUsageFlagBits::eOneTimeSubmit}), vk::Result::eSuccess);
    QCOMPARE(acquireCommand.end(), vk::Result::eSuccess);
    auto acquireFence = m_device->submitCompute(std::move(acquireCommand), FileDescriptor{});
    QVERIFY(acquireFence);

    const QList<VulkanCompositorLayer> changedLayers{
        initialLayers.front(),
        {.rect = QRectF(18, 3, 5, 7), .color = QColor(30, 230, 90, 192)},
    };
    auto changed = compositor->renderTo(target.get(), changedLayers, background, Region(18, 3, 5, 7), std::move(*acquireFence));
    QVERIFY(changed);
    QVERIFY(changed->completionFence.isValid());

    const QImage actual = target->download();
    QImage expected(size, QImage::Format_RGBA8888_Premultiplied);
    expected.fill(background);
    QPainter painter(&expected);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    for (const VulkanCompositorLayer &layer : changedLayers) {
        painter.fillRect(layer.rect, layer.color);
    }
    painter.end();
    QVERIFY(maximumChannelDifference(actual, expected) <= 1);
}

void VulkanTest::testItemRendererExactDamage()
{
    const QSize targetSize(80, 60);
    auto targetTexture = VulkanTexture::allocate(m_device,
                                                 vk::Format::eR8G8B8A8Unorm,
                                                 targetSize,
                                                 vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
                                                 VulkanQueueRole::Compute);
    QVERIFY(targetTexture);

    auto compositor = VulkanCompositor::create(m_device);
    QVERIFY(compositor);
    const QColor preserved(37, 73, 109, 255);
    auto initial = compositor->renderTo(targetTexture.get(), {}, preserved, Region(Rect(QPoint(), targetSize)));
    QVERIFY(initial);

    VulkanRenderTarget vulkanTarget(targetTexture.get(), std::move(initial->completionFence));
    const RenderTarget renderTarget(&vulkanTarget);
    const RenderViewport fractionalViewport(RectF(0, 0, 64, 48), 1.25, renderTarget, QPoint());
    const Region exactTile(16, 16, 16, 16);

    ItemRendererVulkan renderer(m_device);
    QVERIFY(renderer.isValid());
    renderer.beginFrame(renderTarget, fractionalViewport);
    renderer.renderBackground(renderTarget, fractionalViewport, exactTile);
    renderer.endFrame();
    QVERIFY(vulkanTarget.takeCompletionFence().isValid());

    QImage expected(targetSize, QImage::Format_RGBA8888_Premultiplied);
    expected.fill(preserved);
    QPainter painter(&expected);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(static_cast<QRect>(exactTile.boundingRect()), Qt::transparent);
    painter.end();

    const QImage actual = targetTexture->download();
    QVERIFY(!actual.isNull());
    QCOMPARE(maximumChannelDifference(actual, expected), 0);
}

void VulkanTest::testNativeTargetTransforms()
{
    const std::array transforms{
        OutputTransform::Kind::Normal,
        OutputTransform::Kind::Rotate90,
        OutputTransform::Kind::Rotate180,
        OutputTransform::Kind::Rotate270,
        OutputTransform::Kind::FlipX,
        OutputTransform::Kind::FlipX90,
        OutputTransform::Kind::FlipX180,
        OutputTransform::Kind::FlipX270,
    };
    const QSize physicalSize(35, 27);
    const QColor base(21, 37, 59, 255);
    const QColor first(231, 43, 79, 255);
    const QColor last(29, 211, 113, 255);

    for (const OutputTransform::Kind kind : transforms) {
        auto texture = VulkanTexture::allocate(m_device,
                                               vk::Format::eR8G8B8A8Unorm,
                                               physicalSize,
                                               vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
                                               VulkanQueueRole::Compute);
        QVERIFY(texture);
        const OutputTransform transform(kind);
        VulkanRenderTarget vulkanTarget(texture.get(), FileDescriptor{}, transform);
        const RenderTarget target(&vulkanTarget);
        const QSize logicalSize = target.transformedSize();
        const RenderViewport viewport(RectF(QPointF(), QSizeF(logicalSize)), 1.0, target, QPoint());

        auto renderer = std::make_unique<ItemRendererVulkan>(m_device);
        QVERIFY(renderer->isValid());
        renderer->beginFrame(target, viewport);
        renderer->renderBackground(target, viewport, Region(Rect(QPoint(), logicalSize)));
        QPainter *painter = renderer->painter();
        QVERIFY(painter);
        painter->fillRect(QRectF(QPointF(), QSizeF(logicalSize)), base);
        painter->fillRect(QRectF(0, 0, 1, 1), first);
        painter->fillRect(QRectF(logicalSize.width() - 1, logicalSize.height() - 1, 1, 1), last);
        renderer->endFrame();

        const QImage actual = texture->download();
        QVERIFY(!actual.isNull());
        const Rect firstPhysical = transform.map(Rect(0, 0, 1, 1), logicalSize);
        const Rect lastPhysical = transform.map(Rect(logicalSize.width() - 1, logicalSize.height() - 1, 1, 1), logicalSize);
        QCOMPARE(actual.pixelColor(firstPhysical.center()), first);
        QCOMPARE(actual.pixelColor(lastPhysical.center()), last);

        // Damage is already in output-device pixels. Verify that mapping it to
        // target tiles and back is exact for every output transform. This must
        // remain independent of the logical viewport scale; routing it through
        // a fractional-scale viewport would grow the tile by rounding.
        const Region deviceDamage(Rect(5, 7, 4, 3));
        const Region expandedDamage = ItemRendererVulkan::expandDamageToTileBoundaries(target, deviceDamage);
        QVERIFY((deviceDamage - expandedDamage).isEmpty());

        const Region targetDamage = transform.map(deviceDamage, target.transformedSize());
        Region expectedTargetDamage;
        for (const Rect &rect : targetDamage.rects()) {
            const int tileSize = int(VulkanCompositor::TileSize);
            const int left = rect.left() / tileSize * tileSize;
            const int top = rect.top() / tileSize * tileSize;
            const int right = (rect.right() + tileSize - 1) / tileSize * tileSize;
            const int bottom = (rect.bottom() + tileSize - 1) / tileSize * tileSize;
            expectedTargetDamage |= Rect(left, top, right - left, bottom - top);
        }
        expectedTargetDamage &= Rect(QPoint(), physicalSize);
        const Region expandedTargetDamage = transform.map(expandedDamage, target.transformedSize());
        QCOMPARE(expandedTargetDamage, expectedTargetDamage);
    }
}

void VulkanTest::testItemRendererPainterOverlay()
{
    const QSize outputSize(40, 24);
    const RectF logicalRect(10, 20, 20, 12);
    QImage actual(outputSize, QImage::Format_RGBA8888_Premultiplied);
    actual.fill(Qt::transparent);
    const RenderTarget target(&actual);
    const RenderViewport viewport(logicalRect, 2.0, target, QPoint());

    auto renderer = std::make_unique<ItemRendererVulkan>(m_device);
    QVERIFY(renderer->isValid());
    renderer->beginFrame(target, viewport);
    renderer->renderBackground(target, viewport, Region(Rect(QPoint(), outputSize)));
    QPainter *painter = renderer->painter();
    QVERIFY(painter);
    painter->fillRect(QRectF(11, 21, 5, 3), QColor(220, 35, 70, 180));
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(QPen(QColor(20, 210, 90, 220), 1.5));
    painter->drawEllipse(QPointF(23, 25), 3.5, 2.5);
    renderer->endFrame();

    QImage expected(outputSize, QImage::Format_RGBA8888_Premultiplied);
    expected.fill(Qt::transparent);
    QPainter reference(&expected);
    reference.setWindow(logicalRect.toRect());
    reference.fillRect(QRectF(11, 21, 5, 3), QColor(220, 35, 70, 180));
    reference.setRenderHint(QPainter::Antialiasing, true);
    reference.setPen(QPen(QColor(20, 210, 90, 220), 1.5));
    reference.drawEllipse(QPointF(23, 25), 3.5, 2.5);
    reference.end();

    const int difference = maximumChannelDifference(actual, expected);
    QVERIFY2(difference <= 2, qPrintable(QStringLiteral("maximum painter-overlay channel difference was %1").arg(difference)));
}

void VulkanTest::testItemRendererBackdropBlur()
{
    const QSize outputSize(32, 24);
    QImage background(outputSize, QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < outputSize.height(); ++y) {
        for (int x = 0; x < outputSize.width(); ++x) {
            background.setPixelColor(x, y, ((x / 4 + y / 4) % 2) ? QColor(230, 30, 45) : QColor(25, 55, 225));
        }
    }
    auto backgroundTexture = VulkanTexture::upload(m_device,
                                                   background,
                                                   vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
                                                   VulkanQueueRole::Compute);
    QVERIFY(backgroundTexture);
    QImage foreground(1, 1, QImage::Format_RGBA8888_Premultiplied);
    foreground.fill(QColor(20, 230, 80));
    auto foregroundTexture = VulkanTexture::upload(m_device,
                                                   foreground,
                                                   vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
                                                   VulkanQueueRole::Compute);
    QVERIFY(foregroundTexture);

    QImage actual(outputSize, QImage::Format_RGBA8888_Premultiplied);
    actual.fill(Qt::transparent);
    const RenderTarget target(&actual);
    const RenderViewport viewport(RectF(QPointF(), QSizeF(outputSize)), 1.0, target, QPoint());
    const std::array fullVertices{
        QPointF(0, 0),
        QPointF(outputSize.width(), 0),
        QPointF(outputSize.width(), outputSize.height()),
        QPointF(0, outputSize.height()),
    };
    const std::array textureCoordinates{
        QPointF(0, 0),
        QPointF(1, 0),
        QPointF(1, 1),
        QPointF(0, 1),
    };

    auto renderer = std::make_unique<ItemRendererVulkan>(m_device);
    QVERIFY(renderer->isValid());
    renderer->beginFrame(target, viewport);
    renderer->renderBackground(target, viewport, Region(0, 0, outputSize.width(), outputSize.height()));
    renderer->renderTextureQuad(backgroundTexture.get(),
                                fullVertices,
                                textureCoordinates,
                                QRectF(QPointF(), QSizeF(outputSize)),
                                1.0,
                                1.0,
                                1.0);
    renderer->renderBackdropBlur({QRectF(8, 4, 16, 16)}, 2, 2.0, 1.0, 1.0, 0, QMatrix4x4{});
    const std::array foregroundVertices{
        QPointF(12, 8),
        QPointF(16, 8),
        QPointF(16, 12),
        QPointF(12, 12),
    };
    renderer->renderTextureQuad(foregroundTexture.get(),
                                foregroundVertices,
                                textureCoordinates,
                                QRectF(QPointF(), QSizeF(outputSize)),
                                1.0,
                                1.0,
                                1.0);
    auto captureTexture = VulkanTexture::allocate(m_device,
                                                  vk::Format::eR8G8B8A8Unorm,
                                                  outputSize,
                                                  vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc,
                                                  VulkanQueueRole::Compute);
    auto captureCompositor = VulkanCompositor::create(m_device);
    QVERIFY(captureTexture);
    QVERIFY(captureCompositor);
    auto captureResult = renderer->renderCurrentLayersTo(captureCompositor.get(), captureTexture.get(), ColorDescription::sRGB);
    QVERIFY(captureResult);
    renderer->endFrame();

    const QImage captured = captureTexture->download();
    QVERIFY(!captured.isNull());
    QVERIFY(maximumChannelDifference(captured, actual) <= 1);

    QCOMPARE(actual.pixelColor(2, 2), background.pixelColor(2, 2));
    QCOMPARE(actual.pixelColor(29, 21), background.pixelColor(29, 21));
    QCOMPARE(actual.pixelColor(13, 9), QColor(20, 230, 80));
    const QColor blurred = actual.pixelColor(20, 10);
    QVERIFY(blurred.red() > 50 && blurred.red() < 210);
    QVERIFY(blurred.blue() > 50 && blurred.blue() < 210);
    QCOMPARE(blurred.alpha(), 255);

    // Repaint only one compositor tile without changing the scene. The blur
    // kernel samples beyond that tile, so this produces the same pixels only
    // if the undamaged pre-blur backdrop remains available from the first
    // frame.
    const QImage expectedAfterPartialRepaint = actual;
    const Region partialDamage(0, 0, 16, 16);
    renderer->beginFrame(target, viewport);
    renderer->renderBackground(target, viewport, partialDamage);
    renderer->renderTextureQuad(backgroundTexture.get(),
                                fullVertices,
                                textureCoordinates,
                                QRectF(0, 0, 16, 16),
                                1.0,
                                1.0,
                                1.0);
    renderer->renderBackdropBlur({QRectF(8, 4, 8, 12)}, 2, 2.0, 1.0, 1.0, 0, QMatrix4x4{});
    renderer->renderTextureQuad(foregroundTexture.get(),
                                foregroundVertices,
                                textureCoordinates,
                                QRectF(0, 0, 16, 16),
                                1.0,
                                1.0,
                                1.0);
    renderer->endFrame();

    const int partialDifference = maximumChannelDifference(actual, expectedAfterPartialRepaint);
    QVERIFY2(partialDifference <= 1,
             qPrintable(QStringLiteral("maximum cached-blur partial-repaint channel difference was %1").arg(partialDifference)));

    // A damaged part of the backdrop must replace the corresponding cached
    // pixels. Compare a blurred pixel well inside the repaired tile with a
    // fresh full render of the changed scene.
    QImage changedBackground = background;
    QPainter changedBackgroundPainter(&changedBackground);
    changedBackgroundPainter.fillRect(QRect(0, 0, 16, 16), QColor(245, 210, 25));
    changedBackgroundPainter.end();
    auto changedBackgroundTexture = VulkanTexture::upload(m_device,
                                                          changedBackground,
                                                          vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
                                                          VulkanQueueRole::Compute);
    QVERIFY(changedBackgroundTexture);

    renderer->beginFrame(target, viewport);
    renderer->renderBackground(target, viewport, partialDamage);
    renderer->renderTextureQuad(changedBackgroundTexture.get(),
                                fullVertices,
                                textureCoordinates,
                                QRectF(0, 0, 16, 16),
                                1.0,
                                1.0,
                                1.0);
    renderer->renderBackdropBlur({QRectF(8, 4, 8, 12)}, 2, 2.0, 1.0, 1.0, 0, QMatrix4x4{});
    renderer->renderTextureQuad(foregroundTexture.get(),
                                foregroundVertices,
                                textureCoordinates,
                                QRectF(0, 0, 16, 16),
                                1.0,
                                1.0,
                                1.0);
    renderer->endFrame();

    QImage changedReference(outputSize, QImage::Format_RGBA8888_Premultiplied);
    changedReference.fill(Qt::transparent);
    const RenderTarget changedReferenceTarget(&changedReference);
    const RenderViewport changedReferenceViewport(RectF(QPointF(), QSizeF(outputSize)), 1.0, changedReferenceTarget, QPoint());
    auto referenceRenderer = std::make_unique<ItemRendererVulkan>(m_device);
    QVERIFY(referenceRenderer->isValid());
    referenceRenderer->beginFrame(changedReferenceTarget, changedReferenceViewport);
    referenceRenderer->renderBackground(changedReferenceTarget, changedReferenceViewport, Region(0, 0, outputSize.width(), outputSize.height()));
    referenceRenderer->renderTextureQuad(changedBackgroundTexture.get(),
                                         fullVertices,
                                         textureCoordinates,
                                         QRectF(QPointF(), QSizeF(outputSize)),
                                         1.0,
                                         1.0,
                                         1.0);
    referenceRenderer->renderBackdropBlur({QRectF(8, 4, 16, 16)}, 2, 2.0, 1.0, 1.0, 0, QMatrix4x4{});
    referenceRenderer->renderTextureQuad(foregroundTexture.get(),
                                         foregroundVertices,
                                         textureCoordinates,
                                         QRectF(QPointF(), QSizeF(outputSize)),
                                         1.0,
                                         1.0,
                                         1.0);
    referenceRenderer->endFrame();

    const QColor cachedChangedPixel = actual.pixelColor(9, 6);
    const QColor referenceChangedPixel = changedReference.pixelColor(9, 6);
    QVERIFY(std::abs(cachedChangedPixel.red() - referenceChangedPixel.red()) <= 1);
    QVERIFY(std::abs(cachedChangedPixel.green() - referenceChangedPixel.green()) <= 1);
    QVERIFY(std::abs(cachedChangedPixel.blue() - referenceChangedPixel.blue()) <= 1);
    QVERIFY(cachedChangedPixel != expectedAfterPartialRepaint.pixelColor(9, 6));
}

void VulkanTest::testItemRendererNestedTarget()
{
    QImage source(1, 1, QImage::Format_RGBA8888_Premultiplied);
    source.fill(QColor(214, 48, 97, 255));
    auto sourceTexture = VulkanTexture::upload(m_device,
                                               source,
                                               vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
                                               VulkanQueueRole::Compute);
    QVERIFY(sourceTexture);

    const QSize offscreenSize(12, 10);
    auto offscreenTexture = VulkanTexture::allocate(m_device,
                                                    vk::Format::eR8G8B8A8Unorm,
                                                    offscreenSize,
                                                    vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                                                    VulkanQueueRole::Compute);
    QVERIFY(offscreenTexture);
    VulkanRenderTarget offscreenVulkanTarget(offscreenTexture.get());
    const RenderTarget offscreenTarget(&offscreenVulkanTarget);
    const RenderViewport offscreenViewport(RectF(QPointF(), QSizeF(offscreenSize)), 1.0, offscreenTarget, QPoint());
    const std::array offscreenVertices{
        QPointF(0, 0),
        QPointF(offscreenSize.width(), 0),
        QPointF(offscreenSize.width(), offscreenSize.height()),
        QPointF(0, offscreenSize.height()),
    };
    const std::array textureCoordinates{
        QPointF(0, 0),
        QPointF(1, 0),
        QPointF(1, 1),
        QPointF(0, 1),
    };

    auto innerRenderer = std::make_unique<ItemRendererVulkan>(m_device);
    QVERIFY(innerRenderer->isValid());
    innerRenderer->beginFrame(offscreenTarget, offscreenViewport);
    innerRenderer->renderBackground(offscreenTarget, offscreenViewport, Region(Rect(QPoint(), offscreenSize)));
    innerRenderer->renderTextureQuad(sourceTexture.get(),
                                     offscreenVertices,
                                     textureCoordinates,
                                     QRectF(QPointF(), QSizeF(offscreenSize)),
                                     1.0,
                                     1.0,
                                     1.0);
    innerRenderer->endFrame();
    QVERIFY(offscreenVulkanTarget.takeCompletionFence().isValid());

    const QSize outputSize(24, 18);
    QImage actual(outputSize, QImage::Format_RGBA8888_Premultiplied);
    actual.fill(Qt::transparent);
    const RenderTarget outputTarget(&actual);
    const RenderViewport outputViewport(RectF(QPointF(), QSizeF(outputSize)), 1.0, outputTarget, QPoint());
    const std::array deformedVertices{
        QPointF(3, 2),
        QPointF(20, 4),
        QPointF(17, 15),
        QPointF(5, 13),
    };
    auto outerRenderer = std::make_unique<ItemRendererVulkan>(m_device);
    QVERIFY(outerRenderer->isValid());
    outerRenderer->beginFrame(outputTarget, outputViewport);
    outerRenderer->renderBackground(outputTarget, outputViewport, Region(Rect(QPoint(), outputSize)));
    outerRenderer->renderTextureQuad(offscreenTexture.get(),
                                     deformedVertices,
                                     textureCoordinates,
                                     QRectF(QPointF(), QSizeF(outputSize)),
                                     1.0,
                                     1.0,
                                     1.0);
    outerRenderer->endFrame();

    QCOMPARE(actual.pixelColor(11, 8), QColor(214, 48, 97, 255));
    QCOMPARE(actual.pixelColor(1, 1), QColor(0, 0, 0, 0));
    QCOMPARE(actual.pixelColor(22, 16), QColor(0, 0, 0, 0));
}

void VulkanTest::testItemRendererFractionalDebug()
{
    QImage source(QSize(4, 4), QImage::Format_RGBA8888_Premultiplied);
    source.fill(Qt::white);
    auto texture = VulkanTexture::upload(m_device,
                                         source,
                                         vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
                                         VulkanQueueRole::Compute);
    QVERIFY(texture);

    qputenv("KWIN_SCENE_VISUALIZE", "fractional");
    auto renderer = std::make_unique<ItemRendererVulkan>(m_device);
    qunsetenv("KWIN_SCENE_VISUALIZE");
    QVERIFY(renderer->isValid());

    const QSize outputSize(12, 6);
    QImage actual(outputSize, QImage::Format_RGBA8888_Premultiplied);
    actual.fill(Qt::transparent);
    const RenderTarget target(&actual);
    const RenderViewport viewport(RectF(QPointF(), QSizeF(outputSize)), 1.0, target, QPoint());
    const std::array textureCoordinates{
        QPointF(0, 0),
        QPointF(1, 0),
        QPointF(1, 1),
        QPointF(0, 1),
    };
    const std::array alignedVertices{
        QPointF(0, 0),
        QPointF(4, 0),
        QPointF(4, 4),
        QPointF(0, 4),
    };
    const std::array fractionalVertices{
        QPointF(6.25, 0.25),
        QPointF(10.25, 0.25),
        QPointF(10.25, 4.25),
        QPointF(6.25, 4.25),
    };

    renderer->beginFrame(target, viewport);
    renderer->renderBackground(target, viewport, Region(Rect(QPoint(), outputSize)));
    renderer->renderTextureQuad(texture.get(), alignedVertices, textureCoordinates, QRectF(QPointF(), QSizeF(outputSize)), 1, 1, 1);
    renderer->renderTextureQuad(texture.get(), fractionalVertices, textureCoordinates, QRectF(QPointF(), QSizeF(outputSize)), 1, 1, 1);
    renderer->endFrame();

    QCOMPARE(actual.pixelColor(1, 1), QColor(Qt::white));
    const QColor marked = actual.pixelColor(7, 1);
    QVERIFY(marked.red() > marked.green());
    QVERIFY(marked.blue() > marked.green());
    QCOMPARE(marked.alpha(), 255);
}

void VulkanTest::testItemRendererScene()
{
    TestScene scene;

    Item group(scene.root());
    group.setPosition(QPointF(4, 3));
    group.setOpacity(0.75);

    QImage backgroundImage(9, 6, QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < backgroundImage.height(); ++y) {
        for (int x = 0; x < backgroundImage.width(); ++x) {
            backgroundImage.setPixelColor(x, y, QColor(25 + x * 17, 30 + y * 23, 180 - x * 9, 210));
        }
    }
    ImageItem backgroundItem(&group);
    backgroundItem.setImage(backgroundImage);
    backgroundItem.setSize(backgroundImage.size());
    backgroundItem.setZ(-1);

    QImage foregroundImage(4, 4, QImage::Format_RGBA8888_Premultiplied);
    foregroundImage.fill(QColor(230, 80, 25, 170));
    foregroundImage.setPixelColor(1, 1, QColor(20, 240, 90, 255));
    foregroundImage.setPixelColor(2, 2, QColor(20, 80, 245, 128));
    ImageItem foregroundItem(&group);
    foregroundItem.setImage(foregroundImage);
    foregroundItem.setSize(foregroundImage.size());
    foregroundItem.setPosition(QPointF(3, 1));
    foregroundItem.setOpacity(0.6);
    foregroundItem.setZ(1);

    const QSize outputSize(23, 17);
    const Region outputRegion(Rect(QPoint(0, 0), outputSize));
    WindowPaintData paintData;
    paintData.setOpacity(0.8);
    paintData += QPointF(2, 1);

    const auto render = [&](std::unique_ptr<ItemRenderer> renderer) {
        QImage image(outputSize, QImage::Format_RGBA8888_Premultiplied);
        image.fill(QColor(79, 61, 43, 255));
        const RenderTarget target(&image);
        const RenderViewport viewport(RectF(QPointF(0, 0), QSizeF(outputSize)), 1.0, target, QPoint(0, 0));
        ItemRenderer *rendererPointer = renderer.get();
        scene.attachRenderer(std::move(renderer));
        rendererPointer->beginFrame(target, viewport);
        rendererPointer->renderBackground(target, viewport, outputRegion);
        rendererPointer->renderItem(target,
                                    viewport,
                                    scene.root(),
                                    Scene::PAINT_WINDOW_TRANSFORMED,
                                    outputRegion,
                                    paintData,
                                    {},
                                    {});
        rendererPointer->endFrame();
        return image;
    };

    const QImage expected = render(std::make_unique<ItemRendererQPainter>());

    if (m_glContext && m_glContext->makeCurrent()) {
        auto glTargetTexture = GLTexture::allocate(GL_RGBA8, outputSize);
        QVERIFY(glTargetTexture);
        GLFramebuffer glFramebuffer(glTargetTexture.get());
        QVERIFY(glFramebuffer.valid());
        const RenderTarget glTarget(&glFramebuffer);
        const RenderViewport glViewport(RectF(QPointF(0, 0), QSizeF(outputSize)), 1.0, glTarget, QPoint(0, 0));
        auto glRenderer = std::make_unique<ItemRendererOpenGL>(m_glContext->displayObject());
        ItemRenderer *glRendererPointer = glRenderer.get();
        scene.attachRenderer(std::move(glRenderer));
        glRendererPointer->beginFrame(glTarget, glViewport);
        glRendererPointer->renderBackground(glTarget, glViewport, outputRegion);
        glRendererPointer->renderItem(glTarget,
                                      glViewport,
                                      scene.root(),
                                      Scene::PAINT_WINDOW_TRANSFORMED,
                                      outputRegion,
                                      paintData,
                                      {},
                                      {});
        glRendererPointer->endFrame();
        glFinish();
        QImage glActual = glTargetTexture->toImage().mirrored();
        const int glDifference = maximumChannelDifference(glActual, expected);
        QVERIFY2(glDifference <= 3, qPrintable(QStringLiteral("maximum OpenGL scene renderer channel difference was %1").arg(glDifference)));
    }

    auto vulkanRenderer = std::make_unique<ItemRendererVulkan>(m_device);
    QVERIFY(vulkanRenderer->isValid());
    const QImage actual = render(std::move(vulkanRenderer));

    const int difference = maximumChannelDifference(actual, expected);
    QVERIFY2(difference <= 2, qPrintable(QStringLiteral("maximum scene renderer channel difference was %1").arg(difference)));

    auto targetTexture = VulkanTexture::allocate(m_device,
                                                 vk::Format::eR8G8B8A8Unorm,
                                                 outputSize,
                                                 vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
                                                 VulkanQueueRole::Compute);
    QVERIFY(targetTexture);
    VulkanRenderTarget vulkanTarget(targetTexture.get());
    const RenderTarget nativeTarget(&vulkanTarget);
    const RenderViewport nativeViewport(RectF(QPointF(0, 0), QSizeF(outputSize)), 1.0, nativeTarget, QPoint(0, 0));
    auto nativeRenderer = std::make_unique<ItemRendererVulkan>(m_device);
    QVERIFY(nativeRenderer->isValid());
    ItemRenderer *nativeRendererPointer = nativeRenderer.get();
    scene.attachRenderer(std::move(nativeRenderer));
    nativeRendererPointer->beginFrame(nativeTarget, nativeViewport);
    nativeRendererPointer->renderBackground(nativeTarget, nativeViewport, outputRegion);
    nativeRendererPointer->renderItem(nativeTarget,
                                      nativeViewport,
                                      scene.root(),
                                      Scene::PAINT_WINDOW_TRANSFORMED,
                                      outputRegion,
                                      paintData,
                                      {},
                                      {});
    nativeRendererPointer->endFrame();
    QVERIFY(vulkanTarget.takeCompletionFence().isValid());
    QCOMPARE(vulkanTarget.takeRenderTimeQueries().size(), size_t(2));
    const QImage nativeActual = targetTexture->download();
    const int nativeDifference = maximumChannelDifference(nativeActual, expected);
    QVERIFY2(nativeDifference <= 2, qPrintable(QStringLiteral("maximum native scene renderer channel difference was %1").arg(nativeDifference)));
}

void VulkanTest::testItemRendererWindowTransform()
{
    TestScene scene;

    const QColor windowColor(31, 173, 211);
    QImage windowImage(16, 8, QImage::Format_RGBA8888_Premultiplied);
    windowImage.fill(windowColor);
    ImageItem windowItem(scene.root());
    windowItem.setImage(windowImage);
    windowItem.setSize(windowImage.size());
    windowItem.setPosition(QPointF(16.2, 12.2));

    WindowPaintData paintData;
    paintData.setXScale(0.5);
    paintData.setYScale(0.8);
    paintData += QPointF(-4, 4);

    const qreal outputScale = 1.25;
    const QSize logicalSize(48, 32);
    const QSize targetSize = (QSizeF(logicalSize) * outputScale).toSize();
    const Region outputRegion(Rect(QPoint(), targetSize));
    QImage expected(targetSize, QImage::Format_RGBA8888_Premultiplied);
    expected.fill(Qt::transparent);
    QPainter expectedPainter(&expected);
    const QPointF snappedDevicePosition(std::round(windowItem.position().x() * outputScale),
                                        std::round(windowItem.position().y() * outputScale));
    expectedPainter.fillRect(QRectF(snappedDevicePosition + QPointF(paintData.xTranslation(), paintData.yTranslation()) * outputScale,
                                    QSizeF(windowItem.size().width() * paintData.xScale() * outputScale,
                                           windowItem.size().height() * paintData.yScale() * outputScale)),
                             windowColor);
    expectedPainter.end();

    const auto render = [&](std::unique_ptr<ItemRenderer> renderer) {
        QImage image(targetSize, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::transparent);
        const RenderTarget target(&image);
        const RenderViewport viewport(RectF(QPointF(), QSizeF(logicalSize)), outputScale, target, QPoint());
        ItemRenderer *rendererPointer = renderer.get();
        scene.attachRenderer(std::move(renderer));
        rendererPointer->beginFrame(target, viewport);
        rendererPointer->renderBackground(target, viewport, outputRegion);
        rendererPointer->renderItem(target,
                                    viewport,
                                    &windowItem,
                                    Scene::PAINT_WINDOW_TRANSFORMED,
                                    outputRegion,
                                    paintData,
                                    {},
                                    {});
        rendererPointer->endFrame();
        return image;
    };

    if (m_glContext && m_glContext->makeCurrent()) {
        auto glTargetTexture = GLTexture::allocate(GL_RGBA8, targetSize);
        QVERIFY(glTargetTexture);
        GLFramebuffer glFramebuffer(glTargetTexture.get());
        QVERIFY(glFramebuffer.valid());
        const RenderTarget glTarget(&glFramebuffer);
        const RenderViewport glViewport(RectF(QPointF(), QSizeF(logicalSize)), outputScale, glTarget, QPoint());
        auto glRenderer = std::make_unique<ItemRendererOpenGL>(m_glContext->displayObject());
        ItemRenderer *glRendererPointer = glRenderer.get();
        scene.attachRenderer(std::move(glRenderer));
        glRendererPointer->beginFrame(glTarget, glViewport);
        glRendererPointer->renderBackground(glTarget, glViewport, outputRegion);
        glRendererPointer->renderItem(glTarget,
                                      glViewport,
                                      &windowItem,
                                      Scene::PAINT_WINDOW_TRANSFORMED,
                                      outputRegion,
                                      paintData,
                                      {},
                                      {});
        glRendererPointer->endFrame();
        glFinish();
        const QImage glActual = glTargetTexture->toImage().mirrored();
        const int glDifference = maximumChannelDifference(glActual, expected);
        QVERIFY2(glDifference <= 2, qPrintable(QStringLiteral("maximum OpenGL window transform channel difference was %1").arg(glDifference)));
    }

    auto vulkanRenderer = std::make_unique<ItemRendererVulkan>(m_device);
    QVERIFY(vulkanRenderer->isValid());
    const QImage actual = render(std::move(vulkanRenderer));
    const int difference = maximumChannelDifference(actual, expected);
    QVERIFY2(difference <= 2, qPrintable(QStringLiteral("maximum Vulkan window transform channel difference was %1").arg(difference)));
}

void VulkanTest::testDeviceLossRecovery()
{
    auto texture = VulkanTexture::allocate(m_device,
                                           vk::Format::eR8G8B8A8Unorm,
                                           QSize(8, 8),
                                           vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc,
                                           VulkanQueueRole::Compute);
    QVERIFY(texture);

    VulkanDevice *const lostDevice = m_device;
    QSignalSpy lostSpy(lostDevice, &VulkanDevice::deviceLost);
    lostDevice->handleDeviceLoss();
    QCOMPARE(lostSpy.count(), 1);
    QVERIFY(texture->download().isNull());

    QTRY_VERIFY_WITH_TIMEOUT(m_renderDevice->vulkanDevice() != lostDevice, 5000);
    m_device = m_renderDevice->vulkanDevice();
    QVERIFY(m_device);

    auto replacementTexture = VulkanTexture::allocate(m_device,
                                                      vk::Format::eR8G8B8A8Unorm,
                                                      QSize(8, 8),
                                                      vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc,
                                                      VulkanQueueRole::Compute);
    QVERIFY(replacementTexture);
}

} // namespace KWin

WAYLANDTEST_MAIN(KWin::VulkanTest)
#include "vulkan_test.moc"
