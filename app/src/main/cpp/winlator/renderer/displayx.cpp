#include <sys/socket.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/un.h>
#include <string.h>
#include <thread>
#include <mutex>

#include <android/api-level.h>
#include <android/log.h>
#include <android/hardware_buffer.h>
#include <android/surface_control.h>
#include <android/native_window_jni.h>
#include <android/native_window.h>

#include "displayx.hpp"

using PFNASURFACETRANSACTIONSETPOSITION = void (*)(ASurfaceTransaction*, ASurfaceControl*, int32_t, int32_t);
using PFNASURFACETRANSACTIONSETBUFFER = void (*)(ASurfaceTransaction*, ASurfaceControl*, AHardwareBuffer*, int);
using PFNASURFACETRANSACTIONSETGEOMETRY = void (*)(ASurfaceTransaction*, ASurfaceControl*, const ARect&, const ARect&, int32_t);
using PFNASURFACETRANSACTIONSETZORDER = void (*)(ASurfaceTransaction*, ASurfaceControl*, int32_t);
using PFNASURFACETRANSACTIONSETVISIBILITY = void (*)(ASurfaceTransaction*, ASurfaceControl*, enum ASurfaceTransactionVisibility);
using PFNASURFACETRANSACTIONSETBUFFERALPHA = void (*)(ASurfaceTransaction*, ASurfaceControl*, float);
using PFNASURFACETRANSACTIONSETBUFFERTRANSPARENCY = void (*)(ASurfaceTransaction*, ASurfaceControl*, enum ASurfaceTransactionTransparency);
using PFNASURFACETRANSACTIONSTATSGETPRESENTFENCEFD = int (*)(ASurfaceTransactionStats*);
using PFNASURFACETRANSACTIONREPARENT = void (*)(ASurfaceTransaction*, ASurfaceControl*, ASurfaceControl*);
using PFNASURFACETRANSACTIONSETONCOMPLETE = void (*)(ASurfaceTransaction*, void*, ASurfaceTransaction_OnComplete);
using PFNASURFACETRANSACTIONSETONCOMMIT = void (*)(ASurfaceTransaction*, void*, ASurfaceTransaction_OnCommit);
using PFNASURFACETRANSACTIONSETENABLEBACKPRESSURE = void (*)(ASurfaceTransaction*, ASurfaceControl*, bool);
using PFNASURFACETRANSACTIONCREATE = ASurfaceTransaction* (*)();
using PFNASURFACETRANSACTIONDELETE = void (*)(ASurfaceTransaction*);
using PFNASURFACETRANSACTIONAPPLY = void (*)(ASurfaceTransaction*);

using PFNASURFACECONTROLACQUIRE = void (*)(ASurfaceControl*);
using PFNASURFACECONTROLRELEASE = void (*)(ASurfaceControl*);
using PFNASURFACECONTROLCREATE = ASurfaceControl* (*)(ASurfaceControl*, const char*);
using PFNASURFACECONTROLCREATEFROMWINDOW = ASurfaceControl* (*)(ANativeWindow*, const char*);

using PFNACHOREOGRAPHERGETINSTANCE = AChoreographer* (*)();
using PFNACHOREOGRAPHERPOSTFRAMECALLBACK64 = void (*)(AChoreographer*, AChoreographer_frameCallback64, void*);

using PFNAPERFORMANCEHINTGETMANAGER = APerformanceHintManager* (*)();
using PFNAPERFORMANCEHINTCREATESESSION = APerformanceHintSession* (*)(APerformanceHintManager*, const int32_t*, size_t, int64_t);
using PFNAPERFORMANCEHINTREPORTACTUALWORKDURATION = int (*)(APerformanceHintSession*, int64_t);
using PFNAPERFORMANCEHINTUPDATETARGETWORKDURATION = int (*)(APerformanceHintSession*, int64_t);
using PFNAPERFORMANCEHINTCLOSESESSION = void (*)(APerformanceHintSession*);

static PFNASURFACETRANSACTIONSETPOSITION pfnASurfaceTransactionSetPosition = nullptr;
static PFNASURFACETRANSACTIONSETBUFFER pfnASurfaceTransactionSetBuffer = nullptr;
static PFNASURFACETRANSACTIONSETGEOMETRY pfnASurfaceTransactionSetGeometry = nullptr;
static PFNASURFACETRANSACTIONSETZORDER pfnASurfaceTransactionSetZOrder = nullptr;
static PFNASURFACETRANSACTIONSETVISIBILITY pfnASurfaceTransactionSetVisibility = nullptr;
static PFNASURFACETRANSACTIONSETBUFFERALPHA pfnASurfaceTransactionSetBufferAlpha = nullptr;
static PFNASURFACETRANSACTIONSETBUFFERTRANSPARENCY pfnASurfaceTransactionSetBufferTransparency = nullptr;
static PFNASURFACETRANSACTIONREPARENT pfnASurfaceTransactionReparent = nullptr;
static PFNASURFACETRANSACTIONSETONCOMPLETE pfnASurfaceTransactionSetOnComplete = nullptr;
static PFNASURFACETRANSACTIONSETONCOMMIT pfnASurfaceTransactionSetOnCommit = nullptr;
static PFNASURFACETRANSACTIONSETENABLEBACKPRESSURE pfnASurfaceTransactionSetEnableBackPressure = nullptr;
static PFNASURFACETRANSACTIONSTATSGETPRESENTFENCEFD pfnASurfaceTransactionStatsGetPresentFenceFd = nullptr;
static PFNASURFACETRANSACTIONCREATE pfnASurfaceTransactionCreate = nullptr;
static PFNASURFACETRANSACTIONDELETE pfnASurfaceTransactionDelete = nullptr;
static PFNASURFACETRANSACTIONAPPLY pfnASurfaceTransactionApply = nullptr;

static PFNASURFACECONTROLACQUIRE pfnASurfaceControlAcquire = nullptr;
static PFNASURFACECONTROLRELEASE pfnASurfaceControlRelease = nullptr;
static PFNASURFACECONTROLCREATE pfnASurfaceControlCreate = nullptr;
static PFNASURFACECONTROLCREATEFROMWINDOW pfnASurfaceControlCreateFromWindow = nullptr;

static PFNACHOREOGRAPHERGETINSTANCE pfnAChoreographerGetInstance = nullptr;
static PFNACHOREOGRAPHERPOSTFRAMECALLBACK64 pfnAChoreographerPostFrameCallback64 = nullptr;

static PFNAPERFORMANCEHINTGETMANAGER pfnAPerformanceHintGetManager = nullptr;
static PFNAPERFORMANCEHINTCREATESESSION pfnAPerformanceHintCreateSession = nullptr;
static PFNAPERFORMANCEHINTREPORTACTUALWORKDURATION pfnAPerformanceHintReportActualWorkDuration = nullptr;
static PFNAPERFORMANCEHINTUPDATETARGETWORKDURATION pfnAPerformanceHintUpdateTargetWorkDuration = nullptr;
static PFNAPERFORMANCEHINTCLOSESESSION pfnAPerformanceHintCloseSession = nullptr;

void DisplayX::onFrameCallback64(int64_t frameTimeNanos, void* data) {
    auto *self = reinterpret_cast<DisplayX *>(data);
   
    if (!self->env) {
        self->env = self->cache->getEnv();
    }

    if (self->cursorUpdate && self->cursorManager->control && !self->paused) {
        self->updateCursorPosition();
        self->cursorUpdate = false;
    }
    
    pfnAChoreographerPostFrameCallback64(self->choreographer, DisplayX::onFrameCallback64, self);
}

static void sendFD(int& socket, int fd) {
    std::vector<char> control_buffer(CMSG_SPACE(sizeof(int)));

    char dummy = 0;
    struct iovec iov{};
    iov.iov_len = 1;
    iov.iov_base = &dummy;

    struct msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control_buffer.data();
    msg.msg_controllen = control_buffer.size();
                                                                                                             struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    
    *reinterpret_cast<int*>(CMSG_DATA(cmsg)) = fd;

    sendmsg(socket, &msg, 0);
}

static int readFD(int& socket) {
    std::vector<char> msg_contents(1);
    struct iovec iov{};
    iov.iov_base = msg_contents.data();
    iov.iov_len = msg_contents.size();
            
    std::vector<char> control_buf(CMSG_SPACE(sizeof(int)));
    struct msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control_buf.data();
    msg.msg_controllen = control_buf.size();
            
    recvmsg(socket, &msg, MSG_WAITALL);
            
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    int fd = *reinterpret_cast<int*>(CMSG_DATA(cmsg));
            
    return fd;
}

void DisplayX::networkThreadLoop() {
    static constexpr int ADD_CLIENT_SWAPCHAIN = 1;
    static constexpr int PRESENT_IMAGE = 2;
    static constexpr int DESTROY_CLIENT_SWAPCHAIN = 3;
    
    std::array<struct epoll_event, 2> events;
    int n;
    int res;
    int efd;
    int server_fd;
    std::unordered_map<uint8_t, std::unique_ptr<DisplayXSwapchain>> clientSwapchains;
    
    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) 
        printf("Failed to create native rendering socket");
                
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path + 1, "displayx", strlen("displayx"));
    addr.sun_path[0] = '\0';
    socklen_t len = offsetof(struct sockaddr_un, sun_path) + 1 + strlen("displayx");
    res = bind(server_fd, (struct sockaddr *)&addr, len);
    if (res < 0)
        printf("Failed to bind native rendering socket");
               
    res = listen(server_fd, 1);
    if (res < 0)
        printf("Failed to listent to native rendering socket");
                
    efd = epoll_create1(0);
    struct epoll_event event{};
    event.data.fd = server_fd;
    event.events = EPOLLIN;
            
    epoll_ctl(efd, EPOLL_CTL_ADD, server_fd, &event);
           
    while ((n = epoll_wait(efd, events.data(), 2, -1))) {
        if (stopped) {
            close(server_fd);
            clientSwapchains.erase(clientSwapchains.begin(), clientSwapchains.end());
            return;
        }
        
        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == server_fd) {
                if (events[i].events & EPOLLIN) {
                    printf("Received new client connection");
                    int client_fd = accept(server_fd, nullptr, nullptr);
                    struct epoll_event event{};
                    event.data.fd = client_fd;
                    event.events = EPOLLIN;
                    epoll_ctl(efd, EPOLL_CTL_ADD, client_fd, &event);
                }
            } 
            else {
                if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                    printf("Client has disconnected");
                    epoll_ctl(efd, EPOLL_CTL_DEL, events[i].data.fd, nullptr);
                    close(events[i].data.fd);
                    clientSwapchains.erase(clientSwapchains.begin(), clientSwapchains.end());
                    continue;
                }
                
                if (events[i].events & EPOLLIN) {
                    int request_code;
                    int size = read(events[i].data.fd, &request_code, 4);
                    if (size <= 0)
                        continue;
                            
                    switch (request_code) {
                        case ADD_CLIENT_SWAPCHAIN:
                        {
                            uint8_t id;
                            uint32_t imageCount;
                            uint32_t windowId;
                                    
                            read(events[i].data.fd, &id, 1);
                            read(events[i].data.fd, &imageCount, 4);
                            read(events[i].data.fd, &windowId, 4);
                            
                            auto window = windowManager->getWindow(windowId);
                            if (!window)
                                continue;
                                    
                            printf("Received new swapchain from client, id %d images %d", id, imageCount);
                            
                            auto swapchain = std::make_unique<DisplayXSwapchain>();
                            swapchain->id = id;
                            swapchain->window = window;
                            swapchain->images.resize(imageCount);
                                    
                            for (uint32_t j = 0; j < imageCount; j++) {
                                auto drawable = std::make_unique<Drawable>();
                                drawable->id = -1;
                                drawable->textureId = -1;
                                drawable->width = window->width;
                                drawable->height = window->height;
                                drawable->data = nullptr;
                                drawable->isDirty = false;
                                drawable->format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
                                drawable->sizeChanged = false;
                                
                                AHardwareBuffer_recvHandleFromUnixSocket(events[i].data.fd, &drawable->ahb);
                                AHardwareBuffer_Desc outDesc{};
                                AHardwareBuffer_describe(drawable->ahb, &outDesc);
                                drawable->stride = outDesc.stride;
                                drawable->isDirectContent = false;
                                drawable->isDisplayX = true;
                                drawable->drawableObj = nullptr;
                                drawable->sync_fence = -1;
                                
                                swapchain->images[j] = std::move(drawable);
                            }
                             
                            clientSwapchains[id] = std::move(swapchain);
                            break;
                        }    
                        case PRESENT_IMAGE:
                        {
                            uint8_t id;
                            int index;
                            int fence;
                            uint64_t present_id;
                                    
                            read(events[i].data.fd, &id, 1);
                            read(events[i].data.fd, &index, 4);
                            
                            fence = readFD(events[i].data.fd);
                            
                            // Bug fix #5: was events[id].data.fd — id is the
                            // swapchain id (uint8_t), not the epoll event index.
                            // Using it as an index reads the wrong fd and can
                            // go out of bounds on the 2-element events array.
                            read(events[i].data.fd, &present_id, 8);

                            auto swapchain = clientSwapchains[id].get();
                            if (!swapchain)
                                continue;

                            auto drawable = swapchain->images.at(index).get();
                            if (!drawable)
                                continue;

                            printf("[DisplayX] networkThreadLoop: queuing present swapchain=%d imageIndex=%d fence=%d present_id=%llu window=%d",
                                   id, index, fence, (unsigned long long)present_id, swapchain->window->id);

                            auto lock = presentLock.lock();

                            auto presentRequest = std::make_unique<PresentRequest>();
                            presentRequest->drawable = drawable;
                            presentRequest->sync_fence = fence;
                            presentRequest->presentId = present_id;
                            presentRequest->clientFd = events[i].data.fd;
                            presentRequest->window = swapchain->window;
                            presentRequest->swapchainId = id;
                            
                            presentRequests.push(std::move(presentRequest));
                            
                            printf("[DisplayX] networkThreadLoop: presentRequests.size=%zu hasSurface=%d surfaceChanged=%d paused=%d",
                                   presentRequests.size(), (int)hasSurface.load(), (int)surfaceChanged.load(), (int)paused.load());
                            
                            presentLock.notify();
                            break;
                        }    
                        case DESTROY_CLIENT_SWAPCHAIN: {
                            uint8_t id;
                            read(events[i].data.fd, &id, 1);
                            
                            auto swapchain = clientSwapchains[id].get();
                            if (!swapchain)
                                continue;
                            
                            swapchain->window->currentDirectContent = nullptr;
                            clientSwapchains.erase(id);
                            break;
                        }
                        default:
                            break;            
                    }
                }
            }
        }
    }
}

void DisplayX::eventThreadLoop() {
    bool restoreState = false;
    
    while (true) {
        std::function<void()> func = nullptr;
        
        auto lock = eventLock.lock();
        eventLock.wait(lock, [&]{ 
            return stopped || state != State::NONE || !eventQueue.empty();
        });
        
        if (stopped) {
            printf("Received state STOP");
            stopped = true;
            presentLock.notify();
            return;
        }
        
        auto currState = state;
        state = State::NONE;
        
        if (currState == State::PAUSE) {
            printf("Received state PAUSE");
            paused = true;
            eventLock.notify();
        }
            
        if (currState == State::RESUME) {
            printf("Received state RESUME");
            paused = false;
            restoreState = true;
            eventLock.notify();
        }
        
        if (currState == State::CREATE_SURFACE) {
            printf("Received state CREATE_SURFACE");
            createRootWindowControl();
            createRootCursorControl();
            hasSurface = true;
            eventLock.notify();
        }
            
        if (currState == State::CHANGE_SURFACE) {
            printf("Received state CHANGE_SURFACE");
            resizeRootWindow();
            surfaceChanged = true;
            eventLock.notify();
        }
            
        if (hasSurface && restoreState) {
            printf("Restoring state after resume");
            restoreControlState();
            restoreState = false;
        }
            
        if (currState == State::DESTROY_SURFACE) {
            printf("Received state DESTROY_SURFACE");
            hasSurface = false;
            surfaceChanged = false;
            destroyRootCursorControl();
            destroyRootWindowControl();
            eventLock.notify();
        }
        
        if (!eventQueue.empty() && hasSurface && surfaceChanged && !paused) {
            func = eventQueue.front();
            eventQueue.pop();
        }
        
        lock.unlock();
        
        if (func) {
            func();
            {
                auto l = presentLock.lock();
                eventsPending--;
            }
            presentLock.notify();
        }
    }
}

int64_t DisplayX::getCurrentTimeNanos() {
    struct timespec ts{};
    
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

void DisplayX::onCompleteCallback(void *context, ASurfaceTransactionStats *stats) {
    std::unique_ptr<PresentRequest> request(static_cast<PresentRequest *>(context));
    if (request->presentId >= 0) {
        int requestCode = 4;
        write(request->clientFd, &requestCode, 4);
        write(request->clientFd, &request->swapchainId, 1);
        write(request->clientFd, &request->presentId, 8);
    }
}

void DisplayX::presentThreadLoop() {
    ASurfaceTransaction *presentTransaction = pfnASurfaceTransactionCreate();
    JNIEnv *env = cache->getEnv();
    auto lastPresentRequestTimeNanos = 0;
    
    if (isPerformanceHintAPIAvailable()) {
        performanceHintManager = pfnAPerformanceHintGetManager();
        if (!performanceHintManager) {
            printf("[PerfHint] GetManager returned null — device does not expose a PerformanceHintManager; hint session disabled");
        } else {
            printf("[PerfHint] GetManager OK");

            // targetWorkDuration: lower = more aggressive clock boost.
            // Normal mode → full frame budget (e.g. 16.7 ms at 60 Hz).
            // Perf mode   → intentionally tiny target to push the scheduler
            //   to max clocks. The original formula (refreshRate*100) produced
            //   166,666 ns which is below Android's minimum of 1,000,000 ns,
            //   causing CreateSession to silently return null. We clamp to the
            //   documented minimum so the session is created while keeping the
            //   same aggressive-boost behaviour the developer intended.
            static constexpr int64_t PERF_HINT_MIN_TARGET_NS = 1000000LL; // 1 ms
            int64_t frameDurationNs = static_cast<int64_t>(1000000000.0f / xServer->refreshRate);
            int64_t aggressiveTarget = static_cast<int64_t>(1000000000.0f / (xServer->refreshRate * 100.0f));
            int64_t targetWorkDuration = this->perfMode
                ? std::max(aggressiveTarget, PERF_HINT_MIN_TARGET_NS)
                : frameDurationNs;

            int tid = gettid();
            std::vector<int32_t> tids{tid};
            performanceHintSession = pfnAPerformanceHintCreateSession(
                performanceHintManager, tids.data(), tids.size(), targetWorkDuration);

            if (!performanceHintSession) {
                printf("[PerfHint] CreateSession FAILED (returned null) — "
                       "tid=%d targetWorkDuration=%lld ns; hint reporting disabled",
                       tid, (long long)targetWorkDuration);
            } else {
                printf("[PerfHint] CreateSession OK — tid=%d targetWorkDuration=%lld ns "
                       "(frameDuration=%lld ns, perfMode=%s%s)",
                       tid, (long long)targetWorkDuration, (long long)frameDurationNs,
                       this->perfMode ? "ON" : "OFF",
                       (this->perfMode && aggressiveTarget < PERF_HINT_MIN_TARGET_NS)
                           ? " [clamped to 1ms min]" : "");
            }
        }
    } else {
        printf("[PerfHint] API not available at session init — hint session will be skipped");
    }

    while(true) {
        auto lock = presentLock.lock();

        presentLock.wait(lock, [&]{
            return stopped || (eventsPending == 0 && !presentRequests.empty() && hasSurface && surfaceChanged && !paused);
        });

        if (stopped)
            break;

        auto presentRequest = std::move(presentRequests.front());
        presentRequests.pop();
        lock.unlock();

        auto window = presentRequest->window;
        if (!window || !window->control) {
            printf("[DisplayX] presentThreadLoop: skipping — window=%p control=%p",
                   (void*)window, window ? (void*)window->control : nullptr);
            continue;
        }

        auto drawable = presentRequest->drawable;
        if (!drawable) {
            printf("[DisplayX] presentThreadLoop: skipping — drawable is null");
            continue;
        }

        printf("[DisplayX] presentThreadLoop: window=%d enabled=%d isDisplayX=%d ahb=%p fence=%d",
               window->id, (int)window->enabled.load(), (int)drawable->isDisplayX,
               (void*)drawable->ahb, presentRequest->sync_fence);

        // Bug fix #1: measure actual CPU work duration here, in the session's
        // thread, immediately before and after the rendering work.  The commit
        // callback runs on the compositor thread and its wall-clock delta equals
        // the frame interval — NOT the CPU time spent — so it must not report.
        int64_t workStart = getCurrentTimeNanos();

        if (!window->enabled) {
            printf("[DisplayX] presentThreadLoop: window %d disabled — submitting null buffer", window->id);
            pfnASurfaceTransactionSetBufferTransparency(presentTransaction, window->control, ASURFACE_TRANSACTION_TRANSPARENCY_TRANSLUCENT);
            pfnASurfaceTransactionSetBuffer(presentTransaction, window->control, nullptr, presentRequest->sync_fence);
        }
        else if (drawable->isDisplayX) {
            // GPU-rendered buffer (Vulkan layer path).
            // Must use OPAQUE: HWC can overlay GPU-tiled buffers (UBWC/AFBC) directly
            // when they are marked opaque, but rejects them for TRANSLUCENT overlays and
            // falls back to GPU composition — which is exactly what we want to avoid.
            // No GPU_SAMPLED_IMAGE needed because we never hit the GPU composition path.
            //
            // Z-order fix: all X11 window controls default to Z=0. Without an explicit
            // Z-order the Vulkan window can end up behind sibling SurfaceControls
            // (other X11/Wine windows) that happen to be created later. Set Z=1 so the
            // GPU layer is always above regular windows (Z=0) but below the cursor
            // (Z=INT32_MAX).
            if (pfnASurfaceTransactionSetZOrder) {
                printf("[DisplayX] presentThreadLoop: setting Z=1 for window %d", window->id);
                pfnASurfaceTransactionSetZOrder(presentTransaction, window->control, 1);
            } else {
                printf("[DisplayX] presentThreadLoop: WARNING setZOrder is null — z-order fix NOT applied for window %d", window->id);
            }
            printf("[DisplayX] presentThreadLoop: submitting DisplayX buffer ahb=%p fence=%d window=%d",
                   (void*)drawable->ahb, presentRequest->sync_fence, window->id);
            pfnASurfaceTransactionSetBufferTransparency(presentTransaction, window->control, ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE);
            pfnASurfaceTransactionSetBuffer(presentTransaction, window->control, drawable->ahb, presentRequest->sync_fence);

            // Bug fix #7: visibility and geometry must land in the SAME
            // transaction as the buffer. mapWindow()/changeGeometry() apply
            // those through the separate `windowTransaction` object, which
            // has no ordering guarantee relative to `presentTransaction` —
            // SurfaceFlinger can commit this buffer before that SHOW/geometry
            // has arrived (window maps but stays invisible) or after a later
            // windowTransaction apply has overwritten it (stale geometry for
            // a frame). Setting them again here, right before the buffer
            // apply, makes every present atomic and self-contained — the
            // same pattern VulkanRendererScanout::scanoutSetBuffer() uses.
            //
            // Only re-issue the Set* calls when something actually changed
            // since the last frame for this window (tracked in
            // presentGeometryCache), so steady-state presents don't pay for
            // redundant position/visibility writes into the transaction.
            // Guarded: destroyWindowControl() erases entries from the event
            // thread while this runs on the present thread.
            bool geoChanged, visChanged;
            {
                std::lock_guard<std::mutex> cacheLock(presentGeometryCacheMutex);
                auto& geoCache = presentGeometryCache[window];
                geoChanged = !geoCache.sent
                    || geoCache.x != window->x
                    || geoCache.y != window->y
                    || geoCache.width != window->width
                    || geoCache.height != window->height;
                visChanged = !geoCache.sent || geoCache.mapped != window->mapped;

                if (geoChanged) {
                    geoCache.x = window->x;
                    geoCache.y = window->y;
                    geoCache.width = window->width;
                    geoCache.height = window->height;
                }
                if (visChanged) {
                    geoCache.mapped = window->mapped;
                }
                geoCache.sent = true;
            }

            if (geoChanged) {
                printf("[DisplayX] presentThreadLoop: geoChanged window %d pos=(%d,%d) size=%dx%d",
                       window->id, (int)window->x.load(), (int)window->y.load(),
                       (int)window->width.load(), (int)window->height.load());
                if (pfnASurfaceTransactionSetPosition) {
                    pfnASurfaceTransactionSetPosition(presentTransaction, window->control, window->x, window->y);
                }
                else {
                    ARect src{};
                    ARect dst = {
                        .left = window->x,
                        .top = window->y,
                        .right = window->x + window->width,
                        .bottom = window->y + window->height
                    };
                    pfnASurfaceTransactionSetGeometry(presentTransaction, window->control, src, dst, 0);
                }
            }
            if (visChanged) {
                printf("[DisplayX] presentThreadLoop: visChanged window %d mapped=%d",
                       window->id, (int)window->mapped.load());
                pfnASurfaceTransactionSetVisibility(presentTransaction, window->control,
                    window->mapped ? ASURFACE_TRANSACTION_VISIBILITY_SHOW : ASURFACE_TRANSACTION_VISIBILITY_HIDE);
            }

            auto *ptr = presentRequest.release();
            pfnASurfaceTransactionSetOnComplete(presentTransaction, ptr, DisplayX::onCompleteCallback);
            env->CallVoidMethod(xServer->xserverDisplayActivity, cache->updateFrameRating, window->windowObj);
        }
        else {
            // CPU-drawn buffer (normal X11 path) — keep TRANSLUCENT for window compositing.
            printf("[DisplayX] presentThreadLoop: submitting CPU buffer ahb=%p fence=%d window=%d",
                   (void*)drawable->ahb, presentRequest->sync_fence, window->id);
            pfnASurfaceTransactionSetBufferTransparency(presentTransaction, window->control, ASURFACE_TRANSACTION_TRANSPARENCY_TRANSLUCENT);
            pfnASurfaceTransactionSetBuffer(presentTransaction, window->control, drawable->ahb, presentRequest->sync_fence);
        }

        pfnASurfaceTransactionApply(presentTransaction);

        if (isPerformanceHintAPIAvailable() && performanceHintSession) {
            int64_t workDuration = getCurrentTimeNanos() - workStart;
            int ret = pfnAPerformanceHintReportActualWorkDuration(performanceHintSession, workDuration);

            // Only log the first successful report to avoid flooding logcat.
            static bool firstReport = true;
            if (firstReport) {
                if (ret == 0) {
                    printf("[PerfHint] ReportActualWorkDuration OK — first report: workDuration=%lld ns",
                           (long long)workDuration);
                } else {
                    printf("[PerfHint] ReportActualWorkDuration FAILED on first call — ret=%d "
                           "workDuration=%lld ns", ret, (long long)workDuration);
                }
                firstReport = false;
            }
        }
    }

    if (isPerformanceHintAPIAvailable() && performanceHintSession) {
        pfnAPerformanceHintCloseSession(performanceHintSession);
        performanceHintSession = nullptr;
        printf("[PerfHint] Session closed OK");
    }
}

void DisplayX::start() {
    void* handle = dlopen("libandroid.so", RTLD_NOW);
    pfnASurfaceTransactionSetPosition = reinterpret_cast<PFNASURFACETRANSACTIONSETPOSITION>(dlsym(handle,"ASurfaceTransaction_setPosition"));
    printf("[DisplayX] dlsym ASurfaceTransaction_setPosition: %s", pfnASurfaceTransactionSetPosition ? "OK" : "FAILED (will fallback to setGeometry)");
    pfnASurfaceTransactionSetBuffer = reinterpret_cast<PFNASURFACETRANSACTIONSETBUFFER>(dlsym(handle,"ASurfaceTransaction_setBuffer"));
    printf("[DisplayX] dlsym ASurfaceTransaction_setBuffer: %s", pfnASurfaceTransactionSetBuffer ? "OK" : "FAILED (critical!)");
    pfnASurfaceTransactionSetGeometry = reinterpret_cast<PFNASURFACETRANSACTIONSETGEOMETRY>(dlsym(handle,"ASurfaceTransaction_setGeometry"));
    printf("[DisplayX] dlsym ASurfaceTransaction_setGeometry: %s", pfnASurfaceTransactionSetGeometry ? "OK" : "FAILED");
    pfnASurfaceTransactionSetZOrder = reinterpret_cast<PFNASURFACETRANSACTIONSETZORDER>(dlsym(handle,"ASurfaceTransaction_setZOrder"));
    printf("[DisplayX] dlsym ASurfaceTransaction_setZOrder: %s", pfnASurfaceTransactionSetZOrder ? "OK" : "FAILED (null — z-order fix will be skipped)");
    pfnASurfaceTransactionSetVisibility = reinterpret_cast<PFNASURFACETRANSACTIONSETVISIBILITY>(dlsym(handle,"ASurfaceTransaction_setVisibility"));
    pfnASurfaceTransactionReparent = reinterpret_cast<PFNASURFACETRANSACTIONREPARENT>(dlsym(handle,"ASurfaceTransaction_reparent"));
    pfnASurfaceTransactionSetOnComplete = reinterpret_cast<PFNASURFACETRANSACTIONSETONCOMPLETE>(dlsym(handle,"ASurfaceTransaction_setOnComplete"));
    pfnASurfaceTransactionSetOnCommit = reinterpret_cast<PFNASURFACETRANSACTIONSETONCOMMIT>(dlsym(handle,"ASurfaceTransaction_setOnCommit"));
    pfnASurfaceTransactionSetEnableBackPressure = reinterpret_cast<PFNASURFACETRANSACTIONSETENABLEBACKPRESSURE>(dlsym(handle,"ASurfaceTransaction_setEnableBackPressure"));
    pfnASurfaceTransactionSetBufferAlpha = reinterpret_cast<PFNASURFACETRANSACTIONSETBUFFERALPHA>(dlsym(handle, "ASurfaceTransaction_setBufferAlpha"));
    pfnASurfaceTransactionSetBufferTransparency = reinterpret_cast<PFNASURFACETRANSACTIONSETBUFFERTRANSPARENCY>(dlsym(handle, "ASurfaceTransaction_setBufferTransparency"));
    pfnASurfaceTransactionStatsGetPresentFenceFd = reinterpret_cast<PFNASURFACETRANSACTIONSTATSGETPRESENTFENCEFD>(dlsym(handle, "ASurfaceTransactionStats_getPresentFenceFd"));
    pfnASurfaceTransactionCreate = reinterpret_cast<PFNASURFACETRANSACTIONCREATE>(dlsym(handle,"ASurfaceTransaction_create"));
    pfnASurfaceTransactionDelete = reinterpret_cast<PFNASURFACETRANSACTIONDELETE>(dlsym(handle,"ASurfaceTransaction_delete"));
    pfnASurfaceTransactionApply = reinterpret_cast<PFNASURFACETRANSACTIONAPPLY>(dlsym(handle,"ASurfaceTransaction_apply"));

    pfnASurfaceControlAcquire = reinterpret_cast<PFNASURFACECONTROLACQUIRE>(dlsym(handle,"ASurfaceControl_acquire"));
    pfnASurfaceControlRelease = reinterpret_cast<PFNASURFACECONTROLRELEASE>(dlsym(handle,"ASurfaceControl_release"));
    pfnASurfaceControlCreate = reinterpret_cast<PFNASURFACECONTROLCREATE>(dlsym(handle,"ASurfaceControl_create"));
    pfnASurfaceControlCreateFromWindow = reinterpret_cast<PFNASURFACECONTROLCREATEFROMWINDOW>(dlsym(handle,"ASurfaceControl_createFromWindow"));

    pfnAChoreographerGetInstance = reinterpret_cast<PFNACHOREOGRAPHERGETINSTANCE>(dlsym(handle,"AChoreographer_getInstance"));
    pfnAChoreographerPostFrameCallback64 = reinterpret_cast<PFNACHOREOGRAPHERPOSTFRAMECALLBACK64>(dlsym(handle,"AChoreographer_postFrameCallback64"));
    
    pfnAPerformanceHintGetManager = reinterpret_cast<PFNAPERFORMANCEHINTGETMANAGER>(dlsym(handle, "APerformanceHint_getManager"));
    printf("[PerfHint] dlsym APerformanceHint_getManager: %s", pfnAPerformanceHintGetManager ? "OK" : "FAILED (symbol not found)");

    pfnAPerformanceHintCreateSession = reinterpret_cast<PFNAPERFORMANCEHINTCREATESESSION>(dlsym(handle, "APerformanceHint_createSession"));
    printf("[PerfHint] dlsym APerformanceHint_createSession: %s", pfnAPerformanceHintCreateSession ? "OK" : "FAILED (symbol not found)");

    pfnAPerformanceHintReportActualWorkDuration = reinterpret_cast<PFNAPERFORMANCEHINTREPORTACTUALWORKDURATION>(dlsym(handle, "APerformanceHint_reportActualWorkDuration"));
    printf("[PerfHint] dlsym APerformanceHint_reportActualWorkDuration: %s", pfnAPerformanceHintReportActualWorkDuration ? "OK" : "FAILED (symbol not found)");

    pfnAPerformanceHintUpdateTargetWorkDuration = reinterpret_cast<PFNAPERFORMANCEHINTUPDATETARGETWORKDURATION>(dlsym(handle, "APerformanceHint_updateTargetWorkDuration"));
    printf("[PerfHint] dlsym APerformanceHint_updateTargetWorkDuration: %s", pfnAPerformanceHintUpdateTargetWorkDuration ? "OK" : "FAILED (symbol not found)");

    pfnAPerformanceHintCloseSession = reinterpret_cast<PFNAPERFORMANCEHINTCLOSESESSION>(dlsym(handle, "APerformanceHint_closeSession"));
    printf("[PerfHint] dlsym APerformanceHint_closeSession: %s", pfnAPerformanceHintCloseSession ? "OK" : "FAILED (symbol not found)");

    printf("[PerfHint] API fully available: %s", isPerformanceHintAPIAvailable() ? "YES" : "NO (one or more symbols missing — hint session will be skipped)");
        
    eventThread = std::thread(&DisplayX::eventThreadLoop, this);
    networkThread = std::thread(&DisplayX::networkThreadLoop, this);
    presentThread = std::thread(&DisplayX::presentThreadLoop, this);
   
    this->choreographer = pfnAChoreographerGetInstance();
    pfnAChoreographerPostFrameCallback64(this->choreographer, DisplayX::onFrameCallback64, this);
}

void DisplayX::stop() {
    stopped = true;
    eventLock.notify();
}

void DisplayX::pause() {
    auto lock = eventLock.lock();
    state = State::PAUSE;
    eventLock.notify();
    eventLock.wait(lock, [&]{ return state == State::NONE; });
}

void DisplayX::resume() {
    auto lock = eventLock.lock();
    state = State::RESUME;
    eventLock.notify();
    eventLock.wait(lock, [&]{ return state == State::NONE; });
}

void DisplayX::createSurface(ANativeWindow *window) {
    auto lock = eventLock.lock();
    this->native_window = window;
    state = State::CREATE_SURFACE;
    eventLock.notify();
    eventLock.wait(lock, [&]{ return state == State::NONE; });
}

void DisplayX::changeSurface(int width, int height) {
    auto lock = eventLock.lock();
    this->surfaceWidth = width;
    this->surfaceHeight = height;
    state = State::CHANGE_SURFACE;
    eventLock.notify();
    eventLock.wait(lock, [&]{ return state == State::NONE; });
}

void DisplayX::destroySurface() {
    auto lock = eventLock.lock();
    this->native_window = nullptr;
    state = State::DESTROY_SURFACE;
    eventLock.notify();
    eventLock.wait(lock, [&]{ return state == State::NONE; });
}

void DisplayX::queueEvent(std::function<void()> func) {
    auto lock = eventLock.lock();
    {
        auto l = presentLock.lock();
        eventsPending++;
    }
    eventQueue.push(func);
    eventLock.notify();
}

void DisplayX::requestWindowUpdate(Drawable *drawable, Window *window) {
    auto lock = presentLock.lock();
    
    auto presentRequest = std::make_unique<PresentRequest>();
    presentRequest->drawable = drawable;
    presentRequest->sync_fence = -1;
    presentRequest->presentId = -1;
    presentRequest->clientFd = -1;
    presentRequest->window = window;
    
    presentRequests.push(std::move(presentRequest));
    
    presentLock.notify();
}

void DisplayX::requestCursorUpdate() {
    if (!cursorVisible) return;
    
    this->cursorUpdate = true;
}

void DisplayX::createWindowControl(Window *window) {
    if (!window->parent || !window->inputOutput) {
        printf("[DisplayX] createWindowControl: skipping window %d (parent=%p inputOutput=%d)",
               window->id, (void*)window->parent, window->inputOutput);
        return;
    }
        
    window->control = pfnASurfaceControlCreate(window->parent->control, "displayx");
    printf("[DisplayX] createWindowControl: window %d control=%p parent_control=%p",
           window->id, (void*)window->control, (void*)window->parent->control);
    if (pfnASurfaceControlAcquire)    
        pfnASurfaceControlAcquire(window->control);
    
    pfnASurfaceTransactionSetEnableBackPressure(windowTransaction, window->control, false);
    pfnASurfaceTransactionSetVisibility(windowTransaction, window->control, ASURFACE_TRANSACTION_VISIBILITY_HIDE);
    
    if (pfnASurfaceTransactionSetPosition) {
        pfnASurfaceTransactionSetPosition(windowTransaction, window->control, window->x, window->y);
    }
    else {
        ARect src{};
        ARect dst = {
            .left = window->x,
            .top = window->y,
            .right = window->x + window->width,
            .bottom = window->y + window->height
        };
        pfnASurfaceTransactionSetGeometry(windowTransaction, window->control, src, dst, 0);
    }  
    pfnASurfaceTransactionApply(windowTransaction);
}

void DisplayX::destroyWindowControl(Window *window) {
    if (!window) return;

    {
        std::lock_guard<std::mutex> cacheLock(presentGeometryCacheMutex);
        presentGeometryCache.erase(window);
    }

    if (!window->control) return;
 
    pfnASurfaceControlRelease(window->control);
    window->control = nullptr;
}

void DisplayX::mapWindow(Window *window) {
    if (!window->control) {
        printf("[DisplayX] mapWindow: window %d has null control — skipping SHOW", window->id);
        return;
    }
    
    if (window->getClassName() == windowManager->getUnviewableWMClass()) {
        printf("[DisplayX] mapWindow: window %d class='%s' matches unviewable — disabling",
               window->id, window->getClassName().c_str());
        window->enabled = false;
    }
    
    printf("[DisplayX] mapWindow: window %d enabled=%d pos=(%d,%d) size=%dx%d",
           window->id, (int)window->enabled.load(),
           (int)window->x.load(), (int)window->y.load(),
           (int)window->width.load(), (int)window->height.load());
    pfnASurfaceTransactionSetVisibility(windowTransaction, window->control, ASURFACE_TRANSACTION_VISIBILITY_SHOW);
    pfnASurfaceTransactionApply(windowTransaction);
}

void DisplayX::unmapWindow(Window *window) {
    if (!window->control) return;
    
    pfnASurfaceTransactionSetVisibility(windowTransaction, window->control, ASURFACE_TRANSACTION_VISIBILITY_HIDE);
    pfnASurfaceTransactionApply(windowTransaction);
}

void DisplayX::changeGeometry(Window *window, bool resized) {
    if (!window->control) return;
    
    int ret;
    
    if (resized) {
        // Bug fix #6: don't attach the buffer here. On ASurfaceTransaction_setPosition
        // devices the on-screen extent tracks the attached buffer's own dimensions (there's
        // no separate scale/crop call in that branch below), so submitting the buffer here
        // would either (a) show it at the new size before the client has repainted the new
        // dimensions (stale/garbage pixels for a frame), or (b) duplicate the setBuffer+apply
        // that requestWindowUpdate() is about to do for the exact same content moments later
        // once the client's own repaint (ConfigureWindow != PutImage, two separate X11
        // requests) lands. Leaving the previous buffer attached here means the window simply
        // keeps its old on-screen size for one frame until real content arrives — the buffer
        // is submitted exactly once, from the content-update path.
        window->drawable->sizeChanged = false;
    }
    
    if (pfnASurfaceTransactionSetPosition) {
        pfnASurfaceTransactionSetPosition(windowTransaction, window->control, window->x, window->y);
    }
    else {
        ARect src{};
        ARect dst = {
            .left = window->x,
            .top = window->y,
            .right = window->x + window->width,
            .bottom = window->y + window->height
        };
        pfnASurfaceTransactionSetGeometry(windowTransaction, window->control, src, dst, 0);
    }
    
    pfnASurfaceTransactionApply(windowTransaction);
}

void DisplayX::updateCursor(Window *window) {
    int ret;
    
    auto cursor = window->cursor;
    if (!cursor) return;
   
    pfnASurfaceTransactionSetBuffer(cursorTransaction, cursorManager->control, cursor->image->ahb, -1);
    pfnASurfaceTransactionSetVisibility(cursorTransaction, cursorManager->control, (cursor->visible && cursorVisible) ?  ASURFACE_TRANSACTION_VISIBILITY_SHOW : ASURFACE_TRANSACTION_VISIBILITY_HIDE);
    pfnASurfaceTransactionApply(cursorTransaction);
}

void DisplayX::updateCursorPosition() {
    if (!cursorManager->control) return;
    
    jobject pointWindowObj = env->CallObjectMethod(xServer->inputDeviceManager, cache->getPointWindow);
    jint id = env->GetIntField(pointWindowObj, cache->windowID);
    auto pointWindow = windowManager->getWindow(id);
    auto cursor = (pointWindow != nullptr) ? pointWindow->cursor : nullptr;
    auto rootCursor = cursorManager->getRootCursor();
    int x = std::clamp(cursorManager->pointer.posX, 0, windowManager->getRootWindow()->width - 1);
    int y = std::clamp(cursorManager->pointer.posY, 0, windowManager->getRootWindow()->height - 1);
    
    if (cursorVisible || (cursor && cursor->visible)) {
        if (repostCursor) {
            if (cursor != nullptr) {
                pfnASurfaceTransactionSetBuffer(cursorTransaction, cursorManager->control, cursor->image->ahb, -1);
            }
            else {
                pfnASurfaceTransactionSetBuffer(cursorTransaction, cursorManager->control, rootCursor->image->ahb, -1);
            }
            repostCursor = false;
        }
        
        if (pfnASurfaceTransactionSetPosition) {
            pfnASurfaceTransactionSetPosition(cursorTransaction, cursorManager->control, x, y);
        }
        else {
            auto& cursorImage = (cursor != nullptr) ? cursor->image : rootCursor->image;
            ARect src{};
            ARect dst = {
                .left = x,
                .top = y,
                .right = x + cursorImage->width,
                .bottom = y + cursorImage->height
            };
            pfnASurfaceTransactionSetGeometry(cursorTransaction, cursorManager->control, src, dst, 0);
        }
        
        pfnASurfaceTransactionApply(cursorTransaction);
    }
    else {
        pfnASurfaceTransactionSetVisibility(cursorTransaction, cursorManager->control, ASURFACE_TRANSACTION_VISIBILITY_HIDE);
        pfnASurfaceTransactionApply(cursorTransaction);
    }
}

void DisplayX::createRootCursorControl() {
    int ret;
    
    auto rootCursor = cursorManager->getRootCursor();
    auto rootWindow = windowManager->getRootWindow();
    if (!rootCursor || !rootWindow) return;
    
    cursorManager->control = pfnASurfaceControlCreate(rootWindow->control, "displayx");
    if (pfnASurfaceControlAcquire)
        pfnASurfaceControlAcquire(cursorManager->control);
    
    cursorTransaction = pfnASurfaceTransactionCreate();
}

void DisplayX::drawRootCursor() {
    if (!cursorVisible) return;
    
    auto rootCursor = cursorManager->getRootCursor();
    if (!cursorManager) return;
    
    pfnASurfaceTransactionSetBuffer(cursorTransaction, cursorManager->control, rootCursor->image->ahb, -1);
    pfnASurfaceTransactionSetVisibility(cursorTransaction, cursorManager->control, ASURFACE_TRANSACTION_VISIBILITY_SHOW);
    pfnASurfaceTransactionSetZOrder(cursorTransaction, cursorManager->control, INT32_MAX);
    pfnASurfaceTransactionApply(cursorTransaction);
}

void DisplayX::reparentWindow(Window *window, Window *parent) {
    if (!window->control) return;
    
    pfnASurfaceTransactionReparent(windowTransaction, window->control, parent->control);
    pfnASurfaceTransactionApply(windowTransaction);
}

bool DisplayX::isPerformanceHintAPIAvailable() {
    return pfnAPerformanceHintGetManager &&
           pfnAPerformanceHintCreateSession &&
           pfnAPerformanceHintUpdateTargetWorkDuration &&
           pfnAPerformanceHintReportActualWorkDuration &&
           pfnAPerformanceHintCloseSession;
}

void DisplayX::createRootWindowControl() {
    int ret;
    
    auto rootWindow = windowManager->getRootWindow();
    if (!rootWindow) return;
    
    rootWindow->control = pfnASurfaceControlCreateFromWindow(this->native_window, "displayx");
    if (pfnASurfaceControlAcquire)      
        pfnASurfaceControlAcquire(rootWindow->control);
 
    windowTransaction = pfnASurfaceTransactionCreate();
}

void DisplayX::destroyRootWindowControl() {
    this->native_window = nullptr;
    pfnASurfaceTransactionDelete(windowTransaction);
    
    auto rootWindow = windowManager->getRootWindow();
    if (!rootWindow) return;
    if (!rootWindow->control) return;

    pfnASurfaceControlRelease(rootWindow->control);
    rootWindow->control = nullptr;
}

void DisplayX::destroyRootCursorControl() {
    pfnASurfaceTransactionDelete(cursorTransaction);
    
    auto rootCursor = cursorManager->getRootCursor();
    if (!rootCursor) return;
    
    if (rootCursor->image->ahb) {
        AHardwareBuffer_release(rootCursor->image->ahb);
        rootCursor->image->ahb = nullptr;
    }
    
    if (!cursorManager->control) return;

    pfnASurfaceControlRelease(cursorManager->control);
    cursorManager->control = nullptr;
}

void DisplayX::resizeRootWindow() {
    auto rootWindow = windowManager->getRootWindow();
    if (!rootWindow) return;
    
    printf("[DisplayX] resizeRootWindow: surface=%dx%d rootWindow=%dx%d fullscreen=%d",
           surfaceWidth, surfaceHeight,
           (int)rootWindow->width.load(), (int)rootWindow->height.load(), fullscreen);
    
    viewTransformation.update(surfaceWidth, surfaceHeight, rootWindow->width, rootWindow->height);
    
    ARect src{};
    ARect dst{};
    
    if (fullscreen) {
        src.left = viewTransformation.viewOffsetX;
        src.top = viewTransformation.viewOffsetY;
        src.right = viewTransformation.viewOffsetX + viewTransformation.viewWidth;
        src.bottom = viewTransformation.viewOffsetY + viewTransformation.viewHeight;
        
        dst.left = 0;
        dst.top = 0;
        dst.right = surfaceWidth;
        dst.bottom = surfaceHeight;
    }
    else {
        src.left = 0;
        src.top = 0;
        src.right = surfaceWidth;
        src.bottom = surfaceHeight;
        
        dst.left = viewTransformation.viewOffsetX;
        dst.top = viewTransformation.viewOffsetY;
        dst.right = viewTransformation.viewOffsetX + viewTransformation.viewWidth;
        dst.bottom = viewTransformation.viewOffsetY + viewTransformation.viewHeight;
    }
    
    printf("[DisplayX] resizeRootWindow: src=[%d,%d,%d,%d] dst=[%d,%d,%d,%d] ahb=%p",
           src.left, src.top, src.right, src.bottom,
           dst.left, dst.top, dst.right, dst.bottom,
           (void*)rootWindow->drawable->ahb);
    pfnASurfaceTransactionSetGeometry(windowTransaction, rootWindow->control, src, dst, 0);
    pfnASurfaceTransactionSetBuffer(windowTransaction, rootWindow->control, rootWindow->drawable->ahb, -1);
    pfnASurfaceTransactionApply(windowTransaction);
}

void DisplayX::restoreControlState() {
    const auto& windowTree = windowManager->getWindowTree();
    
    for (const auto& entry : windowTree) {
        auto window = entry.second.get();
        if (window == windowManager->getRootWindow()) continue;
        if (!window->control || !window->parent->control) continue;
        
        pfnASurfaceTransactionReparent(windowTransaction, window->control, window->parent->control);
        pfnASurfaceTransactionSetVisibility(windowTransaction, window->control, window->mapped ? ASURFACE_TRANSACTION_VISIBILITY_SHOW : ASURFACE_TRANSACTION_VISIBILITY_HIDE);
        
        if (pfnASurfaceTransactionSetPosition) {
            pfnASurfaceTransactionSetPosition(windowTransaction, window->control, window->x, window->y);
        }
        else {  
            ARect src{};
            ARect dst = {
                .left = window->x,
                .top = window->y,
                .right = window->x + window->width,
                .bottom = window->y + window->height
            };
            pfnASurfaceTransactionSetGeometry(windowTransaction, window->control, src, dst, 0);        
        }
        
        pfnASurfaceTransactionSetBuffer(windowTransaction, window->control, window->enabled ? window->drawable->ahb : nullptr, -1);
        pfnASurfaceTransactionApply(windowTransaction);
    }
    
    repostCursor = true;
    cursorUpdate = true;
}

void DisplayX::toggleFullscreen() {
    auto rootWindow = windowManager->getRootWindow();
    if (!rootWindow) return;
    
    fullscreen = !fullscreen;
    
    ARect src{};
    ARect dst{};
    
    if (fullscreen) {
        src.left = viewTransformation.viewOffsetX;
        src.top = viewTransformation.viewOffsetY;
        src.right = viewTransformation.viewOffsetX + viewTransformation.viewWidth;
        src.bottom = viewTransformation.viewOffsetY + viewTransformation.viewHeight;
        
        dst.left = 0;
        dst.top = 0;
        dst.right = surfaceWidth;
        dst.bottom = surfaceHeight;
    }
    else {
        src.left = 0;
        src.top = 0;
        src.right = surfaceWidth;
        src.bottom = surfaceHeight;
        
        dst.left = viewTransformation.viewOffsetX;
        dst.top = viewTransformation.viewOffsetY;
        dst.right = viewTransformation.viewOffsetX + viewTransformation.viewWidth;
        dst.bottom = viewTransformation.viewOffsetY + viewTransformation.viewHeight;
    }
    
    pfnASurfaceTransactionSetGeometry(windowTransaction, rootWindow->control, src, dst, 0);
    pfnASurfaceTransactionApply(windowTransaction);
}

void DisplayX::setPerformanceMode(bool perfMode) {
    printf("[PerfHint] setPerformanceMode called — mode=%s", perfMode ? "ON" : "OFF");
    this->perfMode = perfMode;
    if (isPerformanceHintAPIAvailable() && performanceHintSession && xServer) {
        static constexpr int64_t PERF_HINT_MIN_TARGET_NS = 1000000LL;
        int64_t frameDurationNs = static_cast<int64_t>(1000000000.0f / xServer->refreshRate);
        int64_t aggressiveTarget = static_cast<int64_t>(1000000000.0f / (xServer->refreshRate * 100.0f));
        int64_t targetWorkDuration = perfMode
            ? std::max(aggressiveTarget, PERF_HINT_MIN_TARGET_NS)
            : frameDurationNs;
        int ret = pfnAPerformanceHintUpdateTargetWorkDuration(performanceHintSession, targetWorkDuration);
        if (ret == 0) {
            printf("[PerfHint] UpdateTargetWorkDuration OK — target=%lld ns (frameDuration=%lld ns%s)",
                   (long long)targetWorkDuration, (long long)frameDurationNs,
                   (perfMode && aggressiveTarget < PERF_HINT_MIN_TARGET_NS) ? ", clamped to 1ms min" : "");
        } else {
            printf("[PerfHint] UpdateTargetWorkDuration FAILED — ret=%d target=%lld ns",
                   ret, (long long)targetWorkDuration);
        }
    } else {
        printf("[PerfHint] UpdateTargetWorkDuration skipped — session not active yet "
               "(will apply at CreateSession time)");
    }
}
