#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gtest/gtest.h>
#include <gui/BufferQueue.h>
#include <gui/Surface.h>
#include <surfacetexture/SurfaceTexture.h>

namespace android {

class SurfaceTextureDiscardTest : public ::testing::Test {
protected:
    void SetUp() override {
        mDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        ASSERT_EQ(EGLBoolean(EGL_TRUE), eglInitialize(mDisplay, nullptr, nullptr));
        const EGLint configAttributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8,
                EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
        EGLint count = 0;
        ASSERT_EQ(EGLBoolean(EGL_TRUE), eglChooseConfig(mDisplay, configAttributes, &mConfig, 1, &count));
        ASSERT_EQ(1, count);
        const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        const EGLint surfaceAttributes[] = {EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_NONE};
        mContext = eglCreateContext(mDisplay, mConfig, EGL_NO_CONTEXT, contextAttributes);
        mSurface = eglCreatePbufferSurface(mDisplay, mConfig, surfaceAttributes);
        ASSERT_NE(EGL_NO_CONTEXT, mContext);
        ASSERT_NE(EGL_NO_SURFACE, mSurface);
        ASSERT_EQ(EGLBoolean(EGL_TRUE), eglMakeCurrent(mDisplay, mSurface, mSurface, mContext));
        glGenTextures(1, &mTextureName);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, mTextureName);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#if COM_ANDROID_GRAPHICS_LIBGUI_FLAGS(WB_CONSUMER_BASE_OWNS_BQ)
        mTexture = new SurfaceTexture(mTextureName, GL_TEXTURE_EXTERNAL_OES, true, false);
        mWindow = mTexture->getSurface();
#else
        sp<IGraphicBufferProducer> producer;
        sp<IGraphicBufferConsumer> consumer;
        BufferQueue::createBufferQueue(&producer, &consumer);
        mTexture = new SurfaceTexture(consumer, mTextureName, GL_TEXTURE_EXTERNAL_OES, true, false);
        mWindow = new Surface(producer, false);
#endif
        ASSERT_EQ(OK, native_window_api_connect(mWindow.get(), NATIVE_WINDOW_API_CPU));
        ASSERT_EQ(OK, native_window_set_buffers_dimensions(mWindow.get(), 8, 8));
        ASSERT_EQ(OK, native_window_set_buffers_format(mWindow.get(), HAL_PIXEL_FORMAT_RGBA_8888));
        ASSERT_EQ(OK, native_window_set_usage(mWindow.get(), GRALLOC_USAGE_SW_WRITE_OFTEN));
    }

    void TearDown() override {
        if (mDisplay != EGL_NO_DISPLAY && mContext != EGL_NO_CONTEXT) {
            eglMakeCurrent(mDisplay, mSurface, mSurface, mContext);
            glFinish();
            if (mTexture != nullptr) mTexture->abandon();
            mWindow.clear();
            mTexture.clear();
            glDeleteTextures(1, &mTextureName);
            eglMakeCurrent(mDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(mDisplay, mContext);
        }
        if (mSurface != EGL_NO_SURFACE) eglDestroySurface(mDisplay, mSurface);
        if (mDisplay != EGL_NO_DISPLAY) eglTerminate(mDisplay);
    }

    void queueColor(uint32_t color, int64_t timestamp) {
        ANativeWindowBuffer* windowBuffer = nullptr;
        ASSERT_EQ(OK, native_window_dequeue_buffer_and_wait(mWindow.get(), &windowBuffer));
        ASSERT_NE(nullptr, windowBuffer);
        sp<GraphicBuffer> buffer = GraphicBuffer::from(windowBuffer);
        void* address = nullptr;
        ASSERT_EQ(OK, buffer->lock(GRALLOC_USAGE_SW_WRITE_OFTEN, &address));
        auto* pixels = static_cast<uint32_t*>(address);
        for (uint32_t row = 0; row < buffer->getHeight(); ++row) {
            for (uint32_t column = 0; column < buffer->getWidth(); ++column) {
                pixels[row * buffer->getStride() + column] = color;
            }
        }
        ASSERT_EQ(OK, buffer->unlock());
        ASSERT_EQ(OK, native_window_set_buffers_timestamp(mWindow.get(), timestamp));
        ANativeWindow* window = mWindow.get();
        ASSERT_EQ(OK, window->queueBuffer(window, windowBuffer, -1));
    }

    void expectColor(unsigned char red, unsigned char green, unsigned char blue) {
        const char* vertexSource = "attribute vec2 position; varying vec2 uv;"
                "void main(){uv=(position+1.0)*0.5;gl_Position=vec4(position,0.0,1.0);}";
        const char* fragmentSource = "#extension GL_OES_EGL_image_external : require\n"
                "precision mediump float; varying vec2 uv; uniform samplerExternalOES image;"
                "void main(){gl_FragColor=texture2D(image,uv);}";
        GLuint vertex = glCreateShader(GL_VERTEX_SHADER);
        GLuint fragment = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(vertex, 1, &vertexSource, nullptr);
        glShaderSource(fragment, 1, &fragmentSource, nullptr);
        glCompileShader(vertex);
        glCompileShader(fragment);
        GLint compiled = GL_FALSE;
        glGetShaderiv(vertex, GL_COMPILE_STATUS, &compiled);
        ASSERT_EQ(GL_TRUE, compiled);
        glGetShaderiv(fragment, GL_COMPILE_STATUS, &compiled);
        ASSERT_EQ(GL_TRUE, compiled);
        GLuint program = glCreateProgram();
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glLinkProgram(program);
        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        ASSERT_EQ(GL_TRUE, linked);
        glUseProgram(program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, mTextureName);
        glUniform1i(glGetUniformLocation(program, "image"), 0);
        const GLfloat vertices[] = {-1, -1, 1, -1, -1, 1, 1, 1};
        GLint position = glGetAttribLocation(program, "position");
        ASSERT_GE(position, 0);
        glVertexAttribPointer(position, 2, GL_FLOAT, GL_FALSE, 0, vertices);
        glEnableVertexAttribArray(position);
        glViewport(0, 0, 8, 8);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        unsigned char pixel[4] = {};
        glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        EXPECT_EQ(red, pixel[0]);
        EXPECT_EQ(green, pixel[1]);
        EXPECT_EQ(blue, pixel[2]);
        EXPECT_EQ(GLenum(GL_NO_ERROR), glGetError());
        glDisableVertexAttribArray(position);
        glUseProgram(0);
        glDeleteProgram(program);
        glDeleteShader(vertex);
        glDeleteShader(fragment);
    }

    EGLDisplay mDisplay = EGL_NO_DISPLAY;
    EGLConfig mConfig = nullptr;
    EGLContext mContext = EGL_NO_CONTEXT;
    EGLSurface mSurface = EGL_NO_SURFACE;
    GLuint mTextureName = 0;
    sp<SurfaceTexture> mTexture;
    sp<Surface> mWindow;
};

TEST_F(SurfaceTextureDiscardTest, EmptyQueuePreservesInitialState) {
    EXPECT_EQ(BufferQueue::NO_BUFFER_AVAILABLE, mTexture->discardNextBuffer());
    EXPECT_EQ(0, mTexture->getTimestamp());
}

TEST_F(SurfaceTextureDiscardTest, DiscardPreservesTextureAndPreviewResumes) {
    ASSERT_NO_FATAL_FAILURE(queueColor(0xff0000ff, 100));
    ASSERT_EQ(OK, mTexture->updateTexImage());
    ASSERT_NO_FATAL_FAILURE(expectColor(255, 0, 0));
    const uint64_t frameNumber = mTexture->getFrameNumber();
    float originalTransform[16];
    mTexture->getTransformMatrix(originalTransform);
    for (int frame = 0; frame < 32; ++frame) {
        if (frame == 16) {
            ASSERT_EQ(OK, native_window_set_buffers_dimensions(mWindow.get(), 16, 8));
        }
        ASSERT_NO_FATAL_FAILURE(queueColor(0xff00ff00, 200 + frame));
        ASSERT_EQ(OK, mTexture->discardNextBuffer());
        EXPECT_EQ(100, mTexture->getTimestamp());
        EXPECT_EQ(frameNumber, mTexture->getFrameNumber());
        float transform[16];
        mTexture->getTransformMatrix(transform);
        for (int component = 0; component < 16; ++component) {
            EXPECT_EQ(originalTransform[component], transform[component]);
        }
        EXPECT_EQ(GLenum(GL_NO_ERROR), glGetError());
    }
    ASSERT_NO_FATAL_FAILURE(expectColor(255, 0, 0));
    EXPECT_EQ(BufferQueue::NO_BUFFER_AVAILABLE, mTexture->discardNextBuffer());
    ASSERT_NO_FATAL_FAILURE(queueColor(0xffff0000, 300));
    ASSERT_EQ(OK, mTexture->updateTexImage());
    EXPECT_EQ(300, mTexture->getTimestamp());
    EXPECT_GT(mTexture->getFrameNumber(), frameNumber);
    ASSERT_NO_FATAL_FAILURE(expectColor(0, 0, 255));
    EXPECT_EQ(GLenum(GL_NO_ERROR), glGetError());
}

TEST_F(SurfaceTextureDiscardTest, DetachedAndAbandonedConsumersRejectDiscard) {
    ASSERT_EQ(OK, mTexture->detachFromContext());
    EXPECT_EQ(INVALID_OPERATION, mTexture->discardNextBuffer());
    mTexture->abandon();
    EXPECT_EQ(NO_INIT, mTexture->discardNextBuffer());
}

TEST_F(SurfaceTextureDiscardTest, OtherContextRejectsDiscardAndOriginalContextResumes) {
    EXPECT_EQ(BufferQueue::NO_BUFFER_AVAILABLE, mTexture->discardNextBuffer());
    const EGLint attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext otherContext = eglCreateContext(mDisplay, mConfig, EGL_NO_CONTEXT, attributes);
    ASSERT_NE(EGL_NO_CONTEXT, otherContext);
    ASSERT_EQ(EGLBoolean(EGL_TRUE), eglMakeCurrent(mDisplay, mSurface, mSurface, otherContext));
    EXPECT_EQ(INVALID_OPERATION, mTexture->discardNextBuffer());
    ASSERT_EQ(EGLBoolean(EGL_TRUE), eglMakeCurrent(mDisplay, mSurface, mSurface, mContext));
    EXPECT_EQ(BufferQueue::NO_BUFFER_AVAILABLE, mTexture->discardNextBuffer());
    EXPECT_EQ(EGLBoolean(EGL_TRUE), eglDestroyContext(mDisplay, otherContext));
}

} // namespace android
