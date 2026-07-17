/*
    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "kwin_wayland_test.h"

#include "core/gpumanager.h"
#include "core/renderdevice.h"
#include "opengl/eglcontext.h"
#include "opengl/glframebuffer.h"
#include "opengl/glshader.h"
#include "opengl/glshadermanager.h"
#include "opengl/gltexture.h"
#include "vulkan/vulkan_compositor.h"
#include "vulkan/vulkan_device.h"
#include "vulkan/vulkan_render_time_query.h"
#include "vulkan/vulkan_texture.h"
#include "wayland_server.h"

#include <QElapsedTimer>
#include <QMatrix4x4>

#include <algorithm>
#include <cerrno>
#include <poll.h>
#include <vector>

namespace KWin
{

static bool waitForCompletion(const FileDescriptor &fence)
{
    pollfd descriptor{
        .fd = fence.get(),
        .events = POLLIN,
        .revents = 0,
    };
    int ret;
    do {
        ret = poll(&descriptor, 1, -1);
    } while (ret < 0 && errno == EINTR);
    return ret == 1 && (descriptor.revents & POLLIN);
}

class VulkanCompositorBenchmark : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void benchmarkOverdraw_data();
    void benchmarkOverdraw();
    void benchmarkOpenGLOverdraw_data();
    void benchmarkOpenGLOverdraw();
    void benchmarkComputeLatencyUnderGraphicsContention_data();
    void benchmarkComputeLatencyUnderGraphicsContention();

private:
    RenderDevice *m_renderDevice = nullptr;
    VulkanDevice *m_device = nullptr;
    std::shared_ptr<EglContext> m_glContext;
};

void VulkanCompositorBenchmark::initTestCase()
{
    qRegisterMetaType<KWin::Window *>();
    QVERIFY(waylandServer()->init(qAppName()));
    kwinApp()->start();

    for (const auto &device : GpuManager::self()->renderDevices()) {
        if (device->vulkanDevice()) {
            m_renderDevice = device.get();
            m_device = device->vulkanDevice();
            break;
        }
    }
    if (!m_device) {
        QSKIP("No Vulkan device available");
    }
    qInfo().nospace() << "Vulkan compute queue: family=" << m_device->computeQueueFamily()
                      << " timestampValidBits=" << m_device->queueFamilyProperties()[m_device->computeQueueFamily()].timestampValidBits
                      << " timestampPeriod=" << m_device->nanosecondsPerQueryTick() << "ns"
                      << " dedicated=" << m_device->hasDedicatedComputeQueue()
                      << " highPriority=" << m_device->hasHighPriorityComputeQueue();
    m_glContext = m_renderDevice->eglContext();
    if (m_glContext) {
        qInfo().nospace() << "OpenGL renderer: " << m_glContext->renderer();
    }
}

void VulkanCompositorBenchmark::benchmarkOverdraw_data()
{
    QTest::addColumn<int>("layerCount");
    QTest::addColumn<qreal>("opacity");
    QTest::newRow("one-translucent-layer") << 1 << 0.08;
    QTest::newRow("sixteen-translucent-layers") << 16 << 0.08;
    QTest::newRow("sixty-four-translucent-layers") << 64 << 0.08;
    QTest::newRow("sixty-four-opaque-layers") << 64 << 1.0;
}

void VulkanCompositorBenchmark::benchmarkOverdraw()
{
    QFETCH(int, layerCount);
    QFETCH(qreal, opacity);
    auto compositor = VulkanCompositor::create(m_device);
    QVERIFY(compositor);

    QImage source(1, 1, QImage::Format_RGBA8888_Premultiplied);
    source.fill(QColor::fromRgbF(0.4, 0.2, 0.1, opacity));
    auto texture = VulkanTexture::upload(m_device,
                                         source,
                                         vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
                                         VulkanQueueRole::Compute);
    QVERIFY(texture);

    QList<VulkanCompositorLayer> layers;
    layers.reserve(layerCount);
    for (int i = 0; i < layerCount; ++i) {
        layers.append(VulkanCompositorLayer{
            .rect = QRectF(0, 0, 1920, 1080),
            .texture = texture.get(),
            .texturePlanes = {texture.get(), nullptr, nullptr},
            .texturePlaneCount = 1,
            .colorDescription = nullptr,
        });
    }

    std::chrono::nanoseconds preprocessDuration;
    std::chrono::nanoseconds compositeDuration;
    QBENCHMARK {
        auto result = compositor->render(QSize(1920, 1080),
                                         layers,
                                         Qt::black,
                                         Region(0, 0, 1920, 1080));
        QVERIFY(result);
        QVERIFY(result->completionFence.isValid());
        QVERIFY(waitForCompletion(result->completionFence));
        if (result->preprocessTime && result->compositeTime) {
            const auto preprocess = result->preprocessTime->gpuDuration();
            const auto composite = result->compositeTime->gpuDuration();
            QVERIFY(preprocess.has_value());
            QVERIFY(composite.has_value());
            preprocessDuration = *preprocess;
            compositeDuration = *composite;
        }
    }
    qInfo().nospace() << "Vulkan compute GPU timestamps: preprocess=" << preprocessDuration.count()
                      << "ns composite=" << compositeDuration.count()
                      << "ns total=" << (preprocessDuration + compositeDuration).count() << "ns";
}

void VulkanCompositorBenchmark::benchmarkOpenGLOverdraw_data()
{
    benchmarkOverdraw_data();
}

void VulkanCompositorBenchmark::benchmarkOpenGLOverdraw()
{
    QFETCH(int, layerCount);
    QFETCH(qreal, opacity);
    if (!m_glContext || !m_glContext->makeCurrent()) {
        QSKIP("No OpenGL context available on the Vulkan render device");
    }

    const QSize outputSize(1920, 1080);
    QImage source(1, 1, QImage::Format_RGBA8888_Premultiplied);
    source.fill(QColor::fromRgbF(0.4, 0.2, 0.1, opacity));
    auto sourceTexture = GLTexture::upload(source);
    auto targetTexture = GLTexture::allocate(GL_RGBA8, outputSize);
    QVERIFY(sourceTexture);
    QVERIFY(targetTexture);
    GLFramebuffer framebuffer(targetTexture.get());
    QVERIFY(framebuffer.valid());
    m_glContext->pushFramebuffer(&framebuffer);

    glViewport(0, 0, outputSize.width(), outputSize.height());
    if (opacity < 1.0) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }

    ShaderBinder binder(ShaderTrait::MapTexture);
    QMatrix4x4 projection;
    projection.ortho(QRectF(QPointF(), QSizeF(outputSize)));
    binder.shader()->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, projection);
    binder.shader()->setUniform(GLShader::IntUniform::Sampler, 0);

    // Warm the static full-screen VBO and shader before collecting timestamps.
    sourceTexture->render(outputSize);
    glFinish();

    GLuint queries[2] = {};
    const bool gpuTimestamps = m_glContext->supportsTimerQueries();
    if (gpuTimestamps) {
        glGenQueries(2, queries);
    }
    std::chrono::nanoseconds gpuDuration;
    QBENCHMARK {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        if (gpuTimestamps) {
            glQueryCounter(queries[0], GL_TIMESTAMP);
        }
        for (int i = 0; i < layerCount; ++i) {
            sourceTexture->render(outputSize);
        }
        if (gpuTimestamps) {
            glQueryCounter(queries[1], GL_TIMESTAMP);
            GLint64 start = 0;
            GLint64 end = 0;
            glGetQueryObjecti64v(queries[0], GL_QUERY_RESULT, &start);
            glGetQueryObjecti64v(queries[1], GL_QUERY_RESULT, &end);
            gpuDuration = std::chrono::nanoseconds(end - start);
        } else {
            glFinish();
        }
    }
    if (gpuTimestamps) {
        glDeleteQueries(2, queries);
        qInfo().nospace() << "OpenGL raster GPU timestamp: composite=" << gpuDuration.count() << "ns";
    } else {
        qInfo() << "OpenGL timer queries unavailable; QBENCHMARK reports synchronized wall time";
    }

    glDisable(GL_BLEND);
    m_glContext->popFramebuffer();
}

void VulkanCompositorBenchmark::benchmarkComputeLatencyUnderGraphicsContention_data()
{
    QTest::addColumn<int>("graphicsLayerCount");
    QTest::newRow("idle") << 0;
    QTest::newRow("sixteen-graphics-layers") << 16;
    QTest::newRow("sixty-four-graphics-layers") << 64;
}

void VulkanCompositorBenchmark::benchmarkComputeLatencyUnderGraphicsContention()
{
    QFETCH(int, graphicsLayerCount);
    if (!m_glContext || !m_glContext->makeCurrent()) {
        QSKIP("No OpenGL context available for generating graphics contention");
    }

    const QSize outputSize(1920, 1080);
    auto compositor = VulkanCompositor::create(m_device);
    QVERIFY(compositor);

    QImage computeSource(1, 1, QImage::Format_RGBA8888_Premultiplied);
    computeSource.fill(QColor::fromRgbF(0.4, 0.2, 0.1, 0.08));
    auto computeTexture = VulkanTexture::upload(m_device,
                                                computeSource,
                                                vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
                                                VulkanQueueRole::Compute);
    QVERIFY(computeTexture);
    QList<VulkanCompositorLayer> computeLayers;
    for (int i = 0; i < 16; ++i) {
        computeLayers.append(VulkanCompositorLayer{
            .rect = QRectF(QPointF(), QSizeF(outputSize)),
            .texture = computeTexture.get(),
            .texturePlanes = {computeTexture.get(), nullptr, nullptr},
            .texturePlaneCount = 1,
            .colorDescription = nullptr,
        });
    }

    QImage graphicsSource(1, 1, QImage::Format_RGBA8888_Premultiplied);
    graphicsSource.fill(QColor::fromRgbF(0.1, 0.25, 0.4, 0.08));
    auto graphicsTexture = GLTexture::upload(graphicsSource);
    auto graphicsTarget = GLTexture::allocate(GL_RGBA8, outputSize);
    QVERIFY(graphicsTexture);
    QVERIFY(graphicsTarget);
    GLFramebuffer framebuffer(graphicsTarget.get());
    QVERIFY(framebuffer.valid());
    m_glContext->pushFramebuffer(&framebuffer);

    glViewport(0, 0, outputSize.width(), outputSize.height());
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    ShaderBinder binder(ShaderTrait::MapTexture);
    QMatrix4x4 projection;
    projection.ortho(QRectF(QPointF(), QSizeF(outputSize)));
    binder.shader()->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, projection);
    binder.shader()->setUniform(GLShader::IntUniform::Sampler, 0);

    // Warm pipeline compilation, static buffers and image allocation before
    // collecting submission-to-completion latency.
    graphicsTexture->render(outputSize);
    glFinish();
    auto warmup = compositor->render(outputSize, computeLayers, Qt::black, Region(0, 0, outputSize.width(), outputSize.height()));
    QVERIFY(warmup);
    QVERIFY(waitForCompletion(warmup->completionFence));

    constexpr int sampleCount = 20;
    std::vector<qint64> completionLatency;
    std::vector<qint64> gpuExecutionTime;
    completionLatency.reserve(sampleCount);
    gpuExecutionTime.reserve(sampleCount);
    for (int sample = 0; sample < sampleCount; ++sample) {
        if (graphicsLayerCount > 0) {
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            for (int i = 0; i < graphicsLayerCount; ++i) {
                graphicsTexture->render(outputSize);
            }
            // Submit the raster workload but deliberately don't synchronize it
            // with Vulkan. This is the contention async compute is intended to
            // make progress through.
            glFlush();
        }

        QElapsedTimer timer;
        timer.start();
        auto result = compositor->render(outputSize, computeLayers, Qt::black, Region(0, 0, outputSize.width(), outputSize.height()));
        QVERIFY(result);
        QVERIFY(waitForCompletion(result->completionFence));
        completionLatency.push_back(timer.nsecsElapsed());
        if (result->preprocessTime && result->compositeTime) {
            const auto preprocess = result->preprocessTime->gpuDuration();
            const auto composite = result->compositeTime->gpuDuration();
            if (preprocess && composite) {
                gpuExecutionTime.push_back((*preprocess + *composite).count());
            }
        }

        // Keep samples independent without including graphics completion in
        // the measured compositor latency.
        glFinish();
    }

    const auto percentile = [](std::vector<qint64> samples, size_t numerator, size_t denominator) {
        std::ranges::sort(samples);
        const size_t index = std::min(samples.size() - 1, (samples.size() * numerator + denominator - 1) / denominator - 1);
        return samples[index];
    };
    const qint64 median = percentile(completionLatency, 1, 2);
    const qint64 p95 = percentile(completionLatency, 95, 100);
    const qint64 gpuMedian = gpuExecutionTime.empty() ? 0 : percentile(gpuExecutionTime, 1, 2);
    qInfo().nospace() << "Async compositor latency: queue="
                      << (m_device->hasDedicatedComputeQueue() ? "compute-only" : "graphics")
                      << " graphicsLayers=" << graphicsLayerCount
                      << " median=" << median << "ns p95=" << p95
                      << "ns gpuExecutionMedian=" << gpuMedian << "ns";
    QTest::setBenchmarkResult(median, QTest::WalltimeNanoseconds);

    glDisable(GL_BLEND);
    m_glContext->popFramebuffer();
}

} // namespace KWin

WAYLANDTEST_MAIN(KWin::VulkanCompositorBenchmark)
#include "vulkan_compositor_benchmark.moc"
