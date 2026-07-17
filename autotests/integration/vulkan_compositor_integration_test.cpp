/*
    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "kwin_wayland_test.h"

#include "backends/virtual/virtual_vulkan_backend.h"
#include "compositor.h"
#include "core/outputconfiguration.h"
#include "core/renderbackend.h"
#include "effect/effecthandler.h"
#include "effect/effectloader.h"
#include "effect/offscreeneffect.h"
#include "effect/offscreenquickview.h"
#include "opengl/glshader.h"
#include "opengl/glshadermanager.h"
#include "scene/imageitem.h"
#include "scene/workspacescene.h"
#include "scripting/windowthumbnailitem.h"
#include "vulkan/vulkan_texture.h"
#include "wayland-client/linuxdmabuf.h"
#include "wayland_server.h"
#include "workspace.h"

#include <KConfigGroup>
#include <KWayland/Client/surface.h>
#include <QPainter>
#include <QQuickWindow>
#include <QRasterWindow>

namespace KWin
{

class VulkanCompositorIntegrationTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void testVirtualOutputFrame();
    void testInternalQuickWindow();
    void testPartialDamageTileExpansion();
    void testWindowThumbnail();
    void testFallApartOffscreenMesh();
    void testGlide3DMesh();
    void testSheet3DMesh();
    void testOffscreenColorFilters();
    void testBackdropBlur();
    void testScreenCaptureEffects();
    void testScreenTransform();
    void testStartupFeedbackOverlay();
    void testCrossFadeSnapshot();
    void testLegacyGlShaderBridge();
    void testLinuxDmabufFeedback();
};

class LegacyGlShaderEffect : public CrossFadeEffect
{
public:
    explicit LegacyGlShaderEffect(EffectWindow *window)
        : m_window(window)
    {
        // Capture first, like AnimationEffect does. The shader is deliberately
        // installed afterwards to exercise the late-shader interop path.
        redirect(window);
        if (!effects->makeOpenGLContextCurrent()) {
            return;
        }
        static const QByteArray fragment = QByteArrayLiteral(R"(
#version 140
uniform sampler2D sampler;
in vec2 texcoord0;
out vec4 fragColor;
void main()
{
    vec4 color = texture(sampler, texcoord0);
    fragColor = vec4(color.b, color.g, color.r, color.a);
}
)");
        m_shader = ShaderManager::instance()->generateCustomShader(ShaderTrait::MapTexture, {}, fragment);
        if (m_shader) {
            setShader(window, m_shader.get());
        }
    }

    bool isActive() const override
    {
        return m_window && m_shader;
    }

    void paintWindow(const RenderTarget &renderTarget,
                     const RenderViewport &viewport,
                     EffectWindow *window,
                     int mask,
                     const Region &deviceRegion,
                     WindowPaintData &data) override
    {
        data.setCrossFadeProgress(0.0);
        effects->paintWindow(renderTarget, viewport, window, mask, deviceRegion, data);
    }

private:
    EffectWindow *const m_window;
    std::unique_ptr<GLShader> m_shader;
};

class BlurTestWindow : public QRasterWindow
{
public:
    BlurTestWindow()
    {
        setFlags(Qt::FramelessWindowHint);
        QSurfaceFormat format;
        format.setAlphaBufferSize(8);
        setFormat(format);
        setProperty("kwin_blur", QVariant::fromValue(RegionF{}));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(QRect(QPoint(), size()), QColor(245, 245, 245, 96));
    }
};

void VulkanCompositorIntegrationTest::initTestCase()
{
    if (!Test::renderNodeAvailable()) {
        QSKIP("No render node available");
    }
    qRegisterMetaType<KWin::Window *>();
    QVERIFY(waylandServer()->init(qAppName()));

    auto config = KSharedConfig::openConfig(QString(), KConfig::SimpleConfig);
    KConfigGroup plugins(config, QStringLiteral("Plugins"));
    for (const QString &name : EffectLoader().listOfKnownEffects()) {
        plugins.writeEntry(name + QStringLiteral("Enabled"), false);
    }
    config->sync();
    kwinApp()->setConfig(config);
    qputenv("KWIN_COMPOSE", QByteArrayLiteral("V"));
    qputenv("KWIN_EFFECTS_FORCE_ANIMATIONS", QByteArrayLiteral("1"));

    kwinApp()->start();
    Test::setOutputConfig({Rect(0, 0, 1280, 1024)});
    QVERIFY(Compositor::self());
    QCOMPARE(Compositor::self()->backend()->compositingType(), VulkanCompositing);
}

void VulkanCompositorIntegrationTest::testLinuxDmabufFeedback()
{
    QVERIFY(Test::setupWaylandConnection(Test::AdditionalWaylandInterface::LinuxDmabuf));
    QVERIFY(Test::linuxDmabuf());
    QVERIFY(Test::waylandSync());
    QTRY_VERIFY_WITH_TIMEOUT(!Test::linuxDmabuf()->mainDevice().isEmpty(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!Test::linuxDmabuf()->formats().isEmpty(), 5000);
    Test::destroyWaylandConnection();
}

void VulkanCompositorIntegrationTest::testVirtualOutputFrame()
{
    const QList<LogicalOutput *> outputs = workspace()->outputs();
    QCOMPARE(outputs.size(), 1);
    const QList<OutputLayer *> layers = Compositor::self()->backend()->compatibleOutputLayers(outputs.front()->backendOutput());
    QCOMPARE(layers.size(), 1);
    auto layer = dynamic_cast<VirtualVulkanLayer *>(layers.front());
    QVERIFY(layer);

    QImage source(8, 8, QImage::Format_RGBA8888_Premultiplied);
    source.fill(QColor(226, 37, 91, 255));
    ImageItem item(kwinApp()->scene()->overlayItem());
    item.setImage(source);
    item.setSize(source.size());
    item.setPosition(QPointF(10, 9));
    kwinApp()->scene()->addRepaintFull();

    QImage frame;
    QTRY_VERIFY_WITH_TIMEOUT(([&]() {
        if (!layer->texture()) {
            return false;
        }
        frame = layer->texture()->download();
        return !frame.isNull() && frame.pixelColor(12, 11).red() >= 224;
    })(),
                             5000);
    QVERIFY(std::abs(frame.pixelColor(12, 11).green() - 37) <= 1);
    QVERIFY(std::abs(frame.pixelColor(12, 11).blue() - 91) <= 1);
    QCOMPARE(frame.pixelColor(12, 11).alpha(), 255);
}

void VulkanCompositorIntegrationTest::testInternalQuickWindow()
{
    const QList<LogicalOutput *> outputs = workspace()->outputs();
    const QList<OutputLayer *> layers = Compositor::self()->backend()->compatibleOutputLayers(outputs.front()->backendOutput());
    auto layer = dynamic_cast<VirtualVulkanLayer *>(layers.front());
    QVERIFY(layer);

    const QColor quickColor(71, 184, 109);
    QQuickWindow window;
    window.setFlags(Qt::FramelessWindowHint);
    window.setColor(quickColor);
    window.setGeometry(640, 120, 240, 160);
    window.show();

    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(window.rendererInterface()->graphicsApi(), QSGRendererInterface::OpenGL, 5000);

    QImage frame;
    QTRY_VERIFY_WITH_TIMEOUT(([&]() {
        if (!layer->texture()) {
            return false;
        }
        frame = layer->texture()->download();
        if (frame.isNull()) {
            return false;
        }
        for (int y = 0; y < frame.height(); y += 8) {
            for (int x = 0; x < frame.width(); x += 8) {
                const QColor actual = frame.pixelColor(x, y);
                if (std::abs(actual.red() - quickColor.red()) <= 2
                    && std::abs(actual.green() - quickColor.green()) <= 2
                    && std::abs(actual.blue() - quickColor.blue()) <= 2) {
                    return true;
                }
            }
        }
        return false;
    })(),
                             5000);
}

void VulkanCompositorIntegrationTest::testPartialDamageTileExpansion()
{
    const QList<LogicalOutput *> outputs = workspace()->outputs();
    QCOMPARE(outputs.size(), 1);
    const QList<OutputLayer *> layers = Compositor::self()->backend()->compatibleOutputLayers(outputs.front()->backendOutput());
    QCOMPARE(layers.size(), 1);
    auto layer = dynamic_cast<VirtualVulkanLayer *>(layers.front());
    QVERIFY(layer);

    const QColor backgroundColor(31, 97, 211, 255);
    QImage backgroundImage(QSize(160, 128), QImage::Format_RGBA8888_Premultiplied);
    backgroundImage.fill(backgroundColor);
    ImageItem background(kwinApp()->scene()->overlayItem());
    background.setImage(backgroundImage);
    background.setSize(backgroundImage.size());
    background.setZ(-2);

    const QColor foregroundColor(229, 53, 79, 255);
    QImage foregroundImage(QSize(64, 48), QImage::Format_RGBA8888_Premultiplied);
    foregroundImage.fill(foregroundColor);
    ImageItem foreground(kwinApp()->scene()->overlayItem());
    foreground.setImage(foregroundImage);
    foreground.setSize(foregroundImage.size());
    foreground.setPosition(QPointF(37, 29));
    foreground.setZ(-1);

    kwinApp()->scene()->addRepaintFull();
    QImage frame;
    QTRY_VERIFY_WITH_TIMEOUT(([&]() {
        frame = layer->texture() ? layer->texture()->download() : QImage{};
        return !frame.isNull()
            && frame.pixelColor(60, 50) == foregroundColor
            && frame.pixelColor(33, 20) == backgroundColor;
    })(),
                             5000);

    // Moving the foreground produces pixel-exact damage whose surrounding
    // 16x16 tile margins still need complete scene contents.
    foreground.setPosition(QPointF(45, 35));
    QTRY_VERIFY_WITH_TIMEOUT(([&]() {
        frame = layer->texture() ? layer->texture()->download() : QImage{};
        return !frame.isNull()
            && frame.pixelColor(104, 50) == foregroundColor
            && frame.pixelColor(39, 50) == backgroundColor;
    })(),
                             5000);
    QCOMPARE(frame.pixelColor(33, 20), backgroundColor);
}

void VulkanCompositorIntegrationTest::testWindowThumbnail()
{
    QVERIFY(Test::setupWaylandConnection());
    {
        std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
        QVERIFY(surface);
        std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
        QVERIFY(shellSurface);
        const QColor topColor(219, 47, 31);
        const QColor bottomColor(27, 83, 219);
        QImage sourceImage(QSize(96, 64), QImage::Format_ARGB32_Premultiplied);
        sourceImage.fill(topColor);
        QPainter sourcePainter(&sourceImage);
        sourcePainter.fillRect(QRect(0, 32, 96, 32), bottomColor);
        sourcePainter.end();

        Window *window = Test::renderAndWaitForShown(surface.get(), sourceImage);
        QVERIFY(window);

        window->move(QPoint(40, 40));

        OffscreenQuickScene thumbnailScene(OffscreenQuickView::ExportMode::Texture);
        QCOMPARE(thumbnailScene.window()->rendererInterface()->graphicsApi(), QSGRendererInterface::OpenGL);
        thumbnailScene.setSource(QUrl::fromLocalFile(QFINDTESTDATA("data/vulkan_thumbnail.qml")),
                                 {{QStringLiteral("testClient"), QVariant::fromValue(window)}});
        QVERIFY(thumbnailScene.rootItem());
        thumbnailScene.setGeometry(Rect(300, 280, 192, 128));

        // The Vulkan compositor must render into a KWin-owned dma-buf that is
        // imported directly by Qt Quick.
        const auto thumbnailSource = WindowThumbnailSource::getOrCreate(thumbnailScene.window(), window);
        Q_EMIT kwinApp()->scene()->preFrameRender();
        thumbnailScene.update(nullptr);
        const auto nativeFrame = thumbnailSource->acquire();
        QVERIFY(nativeFrame.texture);
        QVERIFY(nativeFrame.image.isNull());
        QVERIFY(!nativeFrame.releasePoint.expired());
        kwinApp()->scene()->addRepaintFull();

        const QList<LogicalOutput *> outputs = workspace()->outputs();
        const QList<OutputLayer *> layers = Compositor::self()->backend()->compatibleOutputLayers(outputs.front()->backendOutput());
        auto layer = dynamic_cast<VirtualVulkanLayer *>(layers.front());
        QVERIFY(layer);
        QImage outputFrame;
        QTRY_VERIFY_WITH_TIMEOUT(([&]() {
            if (!layer->texture()) {
                return false;
            }
            outputFrame = layer->texture()->download();
            if (outputFrame.isNull()) {
                return false;
            }
            const auto closeTo = [](const QColor &actual, const QColor &expected) {
                return std::abs(actual.red() - expected.red()) <= 2
                    && std::abs(actual.green() - expected.green()) <= 2
                    && std::abs(actual.blue() - expected.blue()) <= 2;
            };
            return closeTo(outputFrame.pixelColor(396, 300), topColor)
                && closeTo(outputFrame.pixelColor(396, 388), bottomColor);
        })(),
                                 5000);

        // Damage the source after the first Qt Quick frame. This exercises
        // slot reuse and the GL-consumer-to-Vulkan-producer release fence.
        const QColor updatedTopColor(41, 201, 97);
        const QColor updatedBottomColor(229, 190, 37);
        sourceImage.fill(updatedTopColor);
        QPainter updatedPainter(&sourceImage);
        updatedPainter.fillRect(QRect(0, 32, 96, 32), updatedBottomColor);
        updatedPainter.end();
        QSignalSpy damagedSpy(window, &Window::damaged);
        Test::render(surface.get(), sourceImage);
        QTRY_VERIFY_WITH_TIMEOUT(damagedSpy.count() > 0, 5000);

        Q_EMIT kwinApp()->scene()->preFrameRender();
        thumbnailScene.update(nullptr);
        kwinApp()->scene()->addRepaintFull();
        QTRY_VERIFY_WITH_TIMEOUT(([&]() {
            if (!layer->texture()) {
                return false;
            }
            outputFrame = layer->texture()->download();
            if (outputFrame.isNull()) {
                return false;
            }
            const auto closeTo = [](const QColor &actual, const QColor &expected) {
                return std::abs(actual.red() - expected.red()) <= 2
                    && std::abs(actual.green() - expected.green()) <= 2
                    && std::abs(actual.blue() - expected.blue()) <= 2;
            };
            return closeTo(outputFrame.pixelColor(396, 300), updatedTopColor)
                && closeTo(outputFrame.pixelColor(396, 388), updatedBottomColor);
        })(),
                                 5000);
    }
    Test::destroyWaylandConnection();
}

void VulkanCompositorIntegrationTest::testFallApartOffscreenMesh()
{
    QVERIFY(Test::setupWaylandConnection());
    {
        std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
        QVERIFY(surface);
        std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
        QVERIFY(shellSurface);
        Window *window = Test::renderAndWaitForShown(surface.get(), QSize(180, 120), QColor(218, 52, 91));
        QVERIFY(window);
        window->move(QPoint(260, 220));

        QVERIFY(effects->loadEffect(QStringLiteral("fallapart")));
        Effect *effect = effects->findEffect(QStringLiteral("fallapart"));
        QVERIFY(effect);

        QSignalSpy closedSpy(window, &Window::closed);
        shellSurface.reset();
        surface.reset();
        QVERIFY(closedSpy.wait());
        QVERIFY(effect->isActive());

        const QList<LogicalOutput *> outputs = workspace()->outputs();
        const QList<OutputLayer *> layers = Compositor::self()->backend()->compatibleOutputLayers(outputs.front()->backendOutput());
        auto layer = dynamic_cast<VirtualVulkanLayer *>(layers.front());
        QVERIFY(layer);
        QTRY_VERIFY_WITH_TIMEOUT(layer->texture() && !layer->texture()->download().isNull(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!effect->isActive(), 5000);
        effects->unloadEffect(QStringLiteral("fallapart"));
    }
    Test::destroyWaylandConnection();
}

void VulkanCompositorIntegrationTest::testGlide3DMesh()
{
    QVERIFY(Test::setupWaylandConnection());
    {
        QVERIFY(effects->loadEffect(QStringLiteral("glide")));
        Effect *effect = effects->findEffect(QStringLiteral("glide"));
        QVERIFY(effect);

        std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
        QVERIFY(surface);
        std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
        QVERIFY(shellSurface);
        Window *window = Test::renderAndWaitForShown(surface.get(), QSize(180, 120), QColor(57, 122, 231));
        QVERIFY(window);
        window->move(QPoint(360, 180));
        QVERIFY(effect->isActive());

        const QList<OutputLayer *> layers = Compositor::self()->backend()->compatibleOutputLayers(workspace()->outputs().front()->backendOutput());
        auto layer = dynamic_cast<VirtualVulkanLayer *>(layers.front());
        QVERIFY(layer);
        QTRY_VERIFY_WITH_TIMEOUT(layer->texture() && !layer->texture()->download().isNull(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!effect->isActive(), 5000);

        QSignalSpy closedSpy(window, &Window::closed);
        shellSurface.reset();
        surface.reset();
        QVERIFY(closedSpy.wait());
        QVERIFY(effect->isActive());
        QTRY_VERIFY_WITH_TIMEOUT(!effect->isActive(), 5000);
        effects->unloadEffect(QStringLiteral("glide"));
    }
    Test::destroyWaylandConnection();
}

void VulkanCompositorIntegrationTest::testSheet3DMesh()
{
    QVERIFY(Test::setupWaylandConnection(Test::AdditionalWaylandInterface::XdgDialogV1));
    {
        std::unique_ptr<KWayland::Client::Surface> parentSurface(Test::createSurface());
        QVERIFY(parentSurface);
        std::unique_ptr<Test::XdgToplevel> parentToplevel(Test::createXdgToplevelSurface(parentSurface.get()));
        QVERIFY(parentToplevel);
        Window *parent = Test::renderAndWaitForShown(parentSurface.get(), QSize(260, 180), QColor(34, 74, 156));
        QVERIFY(parent);
        parent->move(QPoint(420, 260));

        QVERIFY(effects->loadEffect(QStringLiteral("sheet")));
        Effect *effect = effects->findEffect(QStringLiteral("sheet"));
        QVERIFY(effect);

        std::unique_ptr<KWayland::Client::Surface> childSurface(Test::createSurface());
        QVERIFY(childSurface);
        std::unique_ptr<Test::XdgToplevel> childToplevel(Test::createXdgToplevelSurface(childSurface.get(), [&parentToplevel](Test::XdgToplevel *toplevel) {
            toplevel->set_parent(parentToplevel->object());
        }));
        QVERIFY(childToplevel);
        auto dialog = Test::createXdgDialogV1(childToplevel.get());
        QVERIFY(dialog);
        dialog->set_modal();
        Window *child = Test::renderAndWaitForShown(childSurface.get(), QSize(170, 110), QColor(224, 91, 53));
        QVERIFY(child);
        QVERIFY(child->isModal());
        QVERIFY(effect->isActive());

        const QList<OutputLayer *> layers = Compositor::self()->backend()->compatibleOutputLayers(workspace()->outputs().front()->backendOutput());
        auto layer = dynamic_cast<VirtualVulkanLayer *>(layers.front());
        QVERIFY(layer);
        QTRY_VERIFY_WITH_TIMEOUT(layer->texture() && !layer->texture()->download().isNull(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!effect->isActive(), 5000);

        QSignalSpy closedSpy(child, &Window::closed);
        childSurface.reset();
        QVERIFY(closedSpy.wait());
        QVERIFY(effect->isActive());
        QTRY_VERIFY_WITH_TIMEOUT(!effect->isActive(), 5000);
        dialog.reset();
        childToplevel.reset();
        effects->unloadEffect(QStringLiteral("sheet"));
    }
    Test::destroyWaylandConnection();
}

void VulkanCompositorIntegrationTest::testOffscreenColorFilters()
{
    QVERIFY(Test::setupWaylandConnection());
    {
        const QColor sourceColor(41, 103, 219);
        std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
        QVERIFY(surface);
        std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
        QVERIFY(shellSurface);
        Window *window = Test::renderAndWaitForShown(surface.get(), QSize(180, 120), sourceColor);
        QVERIFY(window);
        window->move(QPoint(700, 320));

        const QList<OutputLayer *> layers = Compositor::self()->backend()->compatibleOutputLayers(workspace()->outputs().front()->backendOutput());
        auto layer = dynamic_cast<VirtualVulkanLayer *>(layers.front());
        QVERIFY(layer);
        const QPoint sample(790, 380);

        QVERIFY(effects->loadEffect(QStringLiteral("invert")));
        Effect *invert = effects->findEffect(QStringLiteral("invert"));
        QVERIFY(invert);
        QVERIFY(QMetaObject::invokeMethod(invert, "toggleScreenInversion"));
        QVERIFY(invert->isActive());
        QImage invertedFrame;
        QTRY_VERIFY_WITH_TIMEOUT(([&]() {
            invertedFrame = layer->texture()->download();
            const QColor pixel = invertedFrame.pixelColor(sample);
            return std::abs(pixel.red() - sourceColor.red()) > 30
                && std::abs(pixel.blue() - sourceColor.blue()) > 30;
        })(),
                                 5000);
        QVERIFY(QMetaObject::invokeMethod(invert, "toggleScreenInversion"));
        QVERIFY(!invert->isActive());
        effects->unloadEffect(QStringLiteral("invert"));

        QVERIFY(effects->loadEffect(QStringLiteral("colorblindnesscorrection")));
        Effect *correction = effects->findEffect(QStringLiteral("colorblindnesscorrection"));
        QVERIFY(correction);
        QVERIFY(correction->isActive());
        QImage correctedFrame;
        QTRY_VERIFY_WITH_TIMEOUT(([&]() {
            correctedFrame = layer->texture()->download();
            const QColor pixel = correctedFrame.pixelColor(sample);
            return std::abs(pixel.red() - sourceColor.red()) > 2
                || std::abs(pixel.green() - sourceColor.green()) > 2
                || std::abs(pixel.blue() - sourceColor.blue()) > 2;
        })(),
                                 5000);
        effects->unloadEffect(QStringLiteral("colorblindnesscorrection"));
    }
    Test::destroyWaylandConnection();
}

void VulkanCompositorIntegrationTest::testBackdropBlur()
{
    QVERIFY(effects->loadEffect(QStringLiteral("blur")));
    Effect *effect = effects->findEffect(QStringLiteral("blur"));
    QVERIFY(effect);
    QVERIFY(effect->isActive());

    QSignalSpy windowAddedSpy(workspace(), &Workspace::windowAdded);
    BlurTestWindow window;
    window.setGeometry(420, 300, 260, 180);
    window.show();
    QTRY_COMPARE(windowAddedSpy.count(), 1);
    kwinApp()->scene()->addRepaintFull();

    const QList<OutputLayer *> layers = Compositor::self()->backend()->compatibleOutputLayers(workspace()->outputs().front()->backendOutput());
    auto layer = dynamic_cast<VirtualVulkanLayer *>(layers.front());
    QVERIFY(layer);
    QTRY_VERIFY_WITH_TIMEOUT(([&]() {
        if (!layer->texture()) {
            return false;
        }
        const QImage frame = layer->texture()->download();
        return !frame.isNull();
    })(),
                             5000);

    window.hide();
    effects->unloadEffect(QStringLiteral("blur"));
}

void VulkanCompositorIntegrationTest::testScreenCaptureEffects()
{
    const QSize outputSize(1280, 1024);
    QImage pattern(outputSize, QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < pattern.height(); ++y) {
        for (int x = 0; x < pattern.width(); ++x) {
            pattern.setPixelColor(x, y, QColor((x / 32 * 47 + y / 32 * 13) % 220 + 24, (x / 32 * 17 + y / 32 * 61) % 220 + 24, (x / 32 * 73 + y / 32 * 29) % 220 + 24));
        }
    }
    ImageItem item(kwinApp()->scene()->overlayItem());
    item.setImage(pattern);
    item.setSize(pattern.size());
    kwinApp()->scene()->addRepaintFull();

    const QList<OutputLayer *> layers = Compositor::self()->backend()->compatibleOutputLayers(workspace()->outputs().front()->backendOutput());
    auto layer = dynamic_cast<VirtualVulkanLayer *>(layers.front());
    QVERIFY(layer);
    QImage baseline;
    QTRY_VERIFY_WITH_TIMEOUT(([&]() {
        baseline = layer->texture() ? layer->texture()->download() : QImage{};
        return !baseline.isNull() && baseline.pixelColor(100, 100) == pattern.pixelColor(100, 100);
    })(),
                             5000);

    const auto changedSampleCount = [&baseline](const QImage &frame) {
        int changed = 0;
        for (int y = 16; y < frame.height(); y += 32) {
            for (int x = 16; x < frame.width(); x += 32) {
                const QColor actual = frame.pixelColor(x, y);
                const QColor expected = baseline.pixelColor(x, y);
                const int difference = std::max({std::abs(actual.red() - expected.red()),
                                                 std::abs(actual.green() - expected.green()),
                                                 std::abs(actual.blue() - expected.blue()),
                                                 std::abs(actual.alpha() - expected.alpha())});
                if (difference > 8) {
                    ++changed;
                }
            }
        }
        return changed;
    };

    QVERIFY(effects->loadEffect(QStringLiteral("zoom")));
    Effect *zoom = effects->findEffect(QStringLiteral("zoom"));
    QVERIFY(zoom);
    QVERIFY(QMetaObject::invokeMethod(zoom, "zoomTo", Q_ARG(double, 2.0)));
    QVERIFY(zoom->isActive());
    QImage zoomed;
    QTRY_VERIFY_WITH_TIMEOUT(([&]() {
        zoomed = layer->texture()->download();
        return changedSampleCount(zoomed) > 100;
    })(),
                             5000);
    QVERIFY(QMetaObject::invokeMethod(zoom, "zoomTo", Q_ARG(double, 1.0)));
    QTRY_VERIFY_WITH_TIMEOUT(!zoom->isActive(), 5000);
    effects->unloadEffect(QStringLiteral("zoom"));

    QVERIFY(effects->loadEffect(QStringLiteral("magnifier")));
    Effect *magnifier = effects->findEffect(QStringLiteral("magnifier"));
    QVERIFY(magnifier);
    QVERIFY(QMetaObject::invokeMethod(magnifier, "toggle"));
    QImage magnified;
    QTRY_VERIFY_WITH_TIMEOUT(([&]() {
        magnified = layer->texture()->download();
        const int changed = changedSampleCount(magnified);
        return changed > 5 && changed < 300;
    })(),
                             5000);
    QVERIFY(QMetaObject::invokeMethod(magnifier, "toggle"));
    QTRY_VERIFY_WITH_TIMEOUT(!magnifier->isActive(), 5000);
    effects->unloadEffect(QStringLiteral("magnifier"));
}

void VulkanCompositorIntegrationTest::testScreenTransform()
{
    QVERIFY(effects->loadEffect(QStringLiteral("screentransform")));
    Effect *effect = effects->findEffect(QStringLiteral("screentransform"));
    QVERIFY(effect);

    LogicalOutput *output = workspace()->outputs().front();
    const QList<OutputLayer *> layers = Compositor::self()->backend()->compatibleOutputLayers(output->backendOutput());
    auto layer = dynamic_cast<VirtualVulkanLayer *>(layers.front());
    QVERIFY(layer);

    QImage marker(QSize(320, 240), QImage::Format_RGBA8888_Premultiplied);
    marker.fill(QColor(29, 71, 203));
    QPainter markerPainter(&marker);
    markerPainter.fillRect(QRect(0, 0, 160, 120), QColor(229, 41, 83));
    markerPainter.end();
    ImageItem item(kwinApp()->scene()->overlayItem());
    item.setImage(marker);
    item.setSize(marker.size());
    kwinApp()->scene()->addRepaintFull();
    QTRY_VERIFY_WITH_TIMEOUT(layer->texture() && !layer->texture()->download().isNull(), 5000);

    OutputConfiguration rotatedConfiguration;
    rotatedConfiguration.changeSet(output->backendOutput())->transform = OutputTransform::Kind::Rotate90;
    workspace()->applyOutputConfiguration(rotatedConfiguration);
    QCOMPARE(output->transform(), OutputTransform(OutputTransform::Kind::Rotate90));
    QVERIFY(effect->isActive());
    QTRY_VERIFY_WITH_TIMEOUT(layer->texture() && !layer->texture()->download().isNull(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!effect->isActive(), 5000);

    OutputConfiguration normalConfiguration;
    normalConfiguration.changeSet(output->backendOutput())->transform = OutputTransform::Kind::Normal;
    workspace()->applyOutputConfiguration(normalConfiguration);
    QCOMPARE(output->transform(), OutputTransform(OutputTransform::Kind::Normal));
    QVERIFY(effect->isActive());
    QTRY_VERIFY_WITH_TIMEOUT(!effect->isActive(), 5000);
    effects->unloadEffect(QStringLiteral("screentransform"));
}

void VulkanCompositorIntegrationTest::testStartupFeedbackOverlay()
{
    QVERIFY(effects->loadEffect(QStringLiteral("startupfeedback")));
    Effect *effect = effects->findEffect(QStringLiteral("startupfeedback"));
    QVERIFY(effect);

    QImage iconImage(24, 24, QImage::Format_RGBA8888_Premultiplied);
    iconImage.fill(QColor(238, 42, 91));
    const QIcon icon(QPixmap::fromImage(iconImage));
    QVERIFY(QMetaObject::invokeMethod(effect,
                                      "gotNewStartup",
                                      Q_ARG(QString, QStringLiteral("vulkan-startup-test")),
                                      Q_ARG(QIcon, icon)));
    QVERIFY(effect->isActive());

    const QList<OutputLayer *> layers = Compositor::self()->backend()->compatibleOutputLayers(workspace()->outputs().front()->backendOutput());
    auto layer = dynamic_cast<VirtualVulkanLayer *>(layers.front());
    QVERIFY(layer);
    QTRY_VERIFY_WITH_TIMEOUT(([&]() {
        if (!layer->texture()) {
            return false;
        }
        const QImage frame = layer->texture()->download();
        for (int y = 0; y < frame.height(); ++y) {
            for (int x = 0; x < frame.width(); ++x) {
                const QColor pixel = frame.pixelColor(x, y);
                if (pixel.red() > 220 && pixel.green() < 70 && pixel.blue() < 120) {
                    return true;
                }
            }
        }
        return false;
    })(),
                             5000);
    QVERIFY(QMetaObject::invokeMethod(effect,
                                      "gotRemoveStartup",
                                      Q_ARG(QString, QStringLiteral("vulkan-startup-test"))));
    QVERIFY(!effect->isActive());
    effects->unloadEffect(QStringLiteral("startupfeedback"));
}

void VulkanCompositorIntegrationTest::testCrossFadeSnapshot()
{
    QVERIFY(Test::setupWaylandConnection());
    {
        std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
        QVERIFY(surface);
        std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
        QVERIFY(shellSurface);
        Window *window = Test::renderAndWaitForShown(surface.get(), QSize(160, 100), QColor(215, 44, 73));
        QVERIFY(window);
        window->move(QPoint(520, 260));

        QVERIFY(effects->loadEffect(QStringLiteral("blendchanges")));
        Effect *effect = effects->findEffect(QStringLiteral("blendchanges"));
        QVERIFY(effect);
        QVERIFY(QMetaObject::invokeMethod(effect, "start", Q_ARG(int, 0)));
        QVERIFY(effect->isActive());

        Test::render(surface.get(), QSize(160, 100), QColor(31, 92, 221));
        Test::flushWaylandConnection();

        const QList<LogicalOutput *> outputs = workspace()->outputs();
        const QList<OutputLayer *> layers = Compositor::self()->backend()->compatibleOutputLayers(outputs.front()->backendOutput());
        auto layer = dynamic_cast<VirtualVulkanLayer *>(layers.front());
        QVERIFY(layer);
        QTRY_VERIFY_WITH_TIMEOUT(layer->texture() && !layer->texture()->download().isNull(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!effect->isActive(), 5000);

        QImage finalFrame;
        QTRY_VERIFY_WITH_TIMEOUT(([&]() {
            finalFrame = layer->texture()->download();
            const QColor center = finalFrame.pixelColor(600, 310);
            return std::abs(center.red() - 31) <= 2
                && std::abs(center.green() - 92) <= 2
                && std::abs(center.blue() - 221) <= 2;
        })(),
                                 5000);
        effects->unloadEffect(QStringLiteral("blendchanges"));
    }
    Test::destroyWaylandConnection();
}

void VulkanCompositorIntegrationTest::testLegacyGlShaderBridge()
{
    QVERIFY(Test::setupWaylandConnection());
    {
        std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
        QVERIFY(surface);
        std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
        QVERIFY(shellSurface);

        const QColor topColor(27, 83, 219);
        const QColor bottomColor(199, 44, 71);
        QImage source(QSize(160, 100), QImage::Format_ARGB32_Premultiplied);
        source.fill(topColor);
        QPainter painter(&source);
        painter.fillRect(QRect(0, 50, 160, 50), bottomColor);
        painter.end();
        Window *window = Test::renderAndWaitForShown(surface.get(), source);
        QVERIFY(window);
        window->move(QPoint(520, 260));

        auto effect = new LegacyGlShaderEffect(window->effectWindow());
        QVERIFY(effect->isActive());
        bool injected = false;
        for (QObject *child : effects->children()) {
            if (qstrcmp(child->metaObject()->className(), "KWin::EffectLoader") == 0) {
                injected = QMetaObject::invokeMethod(child,
                                                     "effectLoaded",
                                                     Q_ARG(KWin::Effect *, effect),
                                                     Q_ARG(QString, QStringLiteral("vulkan-legacy-gl-shader")));
                break;
            }
        }
        QVERIFY(injected);
        QVERIFY(effects->isEffectLoaded(QStringLiteral("vulkan-legacy-gl-shader")));
        kwinApp()->scene()->addRepaintFull();

        const QList<OutputLayer *> layers = Compositor::self()->backend()->compatibleOutputLayers(workspace()->outputs().front()->backendOutput());
        auto layer = dynamic_cast<VirtualVulkanLayer *>(layers.front());
        QVERIFY(layer);
        QImage frame;
        QTRY_VERIFY_WITH_TIMEOUT(([&]() {
            frame = layer->texture() ? layer->texture()->download() : QImage{};
            if (frame.isNull()) {
                return false;
            }
            const QColor top = frame.pixelColor(600, 285);
            const QColor bottom = frame.pixelColor(600, 335);
            return std::abs(top.red() - topColor.blue()) <= 3
                && std::abs(top.green() - topColor.green()) <= 3
                && std::abs(top.blue() - topColor.red()) <= 3
                && std::abs(bottom.red() - bottomColor.blue()) <= 3
                && std::abs(bottom.green() - bottomColor.green()) <= 3
                && std::abs(bottom.blue() - bottomColor.red()) <= 3;
        })(),
                                 5000);

        effects->unloadEffect(QStringLiteral("vulkan-legacy-gl-shader"));
    }
    Test::destroyWaylandConnection();
}

} // namespace KWin

WAYLANDTEST_MAIN(KWin::VulkanCompositorIntegrationTest)
#include "vulkan_compositor_integration_test.moc"
