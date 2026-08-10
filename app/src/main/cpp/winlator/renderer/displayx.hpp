#pragma once

#include <string>
#include <algorithm>
#include <thread>
#include <functional>
#include <queue>
#include <cmath>
#include <dlfcn.h>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <android/choreographer.h>
#include <android/performance_hint.h>

#include "renderer_jni.hpp"
#include "view_transformation.hpp"
#include "window.hpp"
#include "cursor.hpp"

class DisplayX {
    private:
        enum class State {
            NONE,
            PAUSE,
            RESUME,
            CREATE_SURFACE,
            DESTROY_SURFACE,
            CHANGE_SURFACE
        };
        
        struct DisplayXLock {
           std::condition_variable cv;
           std::mutex mutex;
           
           std::unique_lock<std::mutex> lock() {
               return std::unique_lock<std::mutex>(mutex);
           }
           
           template<typename Predicate>
           void wait(std::unique_lock<std::mutex>& lock, Predicate pred) {
               cv.wait(lock, pred);
           }
           
           void notify() {
               cv.notify_all();
           }
        };
        
        struct PresentRequest {
            Drawable *drawable;
            int sync_fence;
            uint64_t presentId;
            uint8_t swapchainId;
            int clientFd;
            Window *window;
        };
        
        struct DisplayXSwapchain {
            uint8_t id;
            Window *window;
            std::vector<std::unique_ptr<Drawable>> images;
        };
        
        // Tracks what was last sent to presentTransaction for a given window's
        // direct-scanout control, so presentThreadLoop only re-issues
        // SetPosition/SetGeometry/SetVisibility when the value actually
        // changed since the previous frame instead of every present.
        struct PresentGeometryCache {
            int32_t x = 0;
            int32_t y = 0;
            int32_t width = 0;
            int32_t height = 0;
            bool mapped = false;
            bool sent = false;
        };
        
        JNIEnv *env = nullptr;
        int surfaceWidth = 0;
        int surfaceHeight = 0;
        AChoreographer *choreographer = nullptr;
        ViewTransformation viewTransformation;
        ANativeWindow *native_window = nullptr;
        
        APerformanceHintManager *performanceHintManager = nullptr;
        APerformanceHintSession *performanceHintSession = nullptr;
        
        DisplayXLock eventLock;
        DisplayXLock presentLock;
        
        ASurfaceTransaction *windowTransaction = nullptr;
        ASurfaceTransaction *cursorTransaction = nullptr;
        std::mutex presentGeometryCacheMutex;
        std::unordered_map<Window*, PresentGeometryCache> presentGeometryCache;
        std::queue<std::unique_ptr<PresentRequest>> presentRequests;
        std::queue<std::function<void()>> eventQueue;
        
        std::thread eventThread;
        std::thread networkThread;
        std::thread presentThread;
        
        State state = State::NONE;
        std::atomic_bool paused{false};
        std::atomic_bool stopped{false};
        std::atomic_bool hasSurface{false};
        std::atomic_bool surfaceChanged{false};
        std::atomic_bool perfMode{true};
        
        std::atomic_bool cursorUpdate{false};
        std::atomic_bool repostCursor{false};
        bool fullscreen = false;
        int eventsPending = 0;
        int64_t previousReportedWorkTime = 0;
        
        void eventThreadLoop();
        void networkThreadLoop();
        void presentThreadLoop();
        static void onFrameCallback64(int64_t frameTimeNanos, void *data);
        static void onCompleteCallback(void *context, ASurfaceTransactionStats *stats);
        int64_t getCurrentTimeNanos();
        bool isPerformanceHintAPIAvailable();
        
        void createRootWindowControl();
        void createRootCursorControl();
        void resizeRootWindow();
        void destroyRootWindowControl();
        void destroyRootCursorControl();
        void restoreControlState();
        
    public:
        WindowManager *windowManager = nullptr;
        CursorManager *cursorManager = nullptr;
        JNIXServer *xServer = nullptr;
        JNICache *cache = nullptr;
        
        std::atomic_bool cursorVisible{false};
        
        void start();
        void createSurface(ANativeWindow *window);
        void changeSurface(int width, int height);
        void destroySurface();
        void stop();
        void pause();
        void resume();
        
        void queueEvent(std::function<void()> func);
        void requestWindowUpdate(Drawable *drawable, Window *window);
        void requestCursorUpdate();
        void updateCursorPosition();
        
        void createWindowControl(Window *window);
        void destroyWindowControl(Window *window);
        void mapWindow(Window *window);
        void unmapWindow(Window *window);
        void changeGeometry(Window *window, bool resized);
        void reparentWindow(Window *window, Window *parent);
        void updateCursor(Window *window);
        void drawRootCursor();
        void toggleFullscreen();
        void setPerformanceMode(bool perfMode);
};