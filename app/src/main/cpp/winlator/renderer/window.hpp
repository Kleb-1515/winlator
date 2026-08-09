#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <mutex>

#include "drawable.hpp"
#include "renderer_jni.hpp"
#include "cursor.hpp"

struct Window {
    int id;
    // x/y/width/height/mapped/enabled are written from the X-server request
    // thread (see renderer_jni.cpp: nativeMapWindow, nativeUnmapWindow,
    // nativeUpdateWindowGeometry) and read every frame from DisplayX's
    // present thread (and separately from its event thread) with no lock in
    // between — atomic keeps each individual read/write well-defined. This
    // does not make a (x,y,width,height) group read atomic as a whole; see
    // note in displayx.cpp's presentThreadLoop.
    std::atomic<int32_t> width{0};
    std::atomic<int32_t> height{0};
    std::atomic<int32_t> x{0};
    std::atomic<int32_t> y{0};
    std::atomic<bool> mapped{false};
    std::atomic<bool> enabled{false};
    std::string className;
    bool inputOutput;
    bool hasContent;
    std::unique_ptr<struct Drawable> drawable;
    Window *parent;
    Cursor *cursor;
    std::vector<Window *> children;
    jobject attributes;
    jobject windowObj;
    std::unordered_map<int, std::unique_ptr<struct Drawable>> directContents;
    Drawable *currentDirectContent;
    ASurfaceControl *control;

    // className is a std::string — can't be made atomic like the fields
    // above. It's written from the X-server thread (nativeSetWindowClassName)
    // and read from DisplayX's event thread (mapWindow()'s unviewable-class
    // check), so raw field access from either side is a genuine data race
    // (not just a torn value — a concurrent std::string mutation can hand a
    // reader a dangling buffer). Route all access through these two methods.
    mutable std::mutex classNameMutex;

    std::string getClassName() const {
        std::lock_guard<std::mutex> lock(classNameMutex);
        return className;
    }

    void setClassName(const std::string& name) {
        std::lock_guard<std::mutex> lock(classNameMutex);
        className = name;
    }
        
    bool hasDirectContents() {
        return !directContents.empty();
    }
    
    int getRootX() {
        int rootX = x;
        auto window = parent;
        while (window != nullptr) {
            rootX += window->x;
            window = window->parent;
        }
        return rootX;
    }
    
    int getRootY() {
        int rootY = y;
        auto window = parent;
        while (window != nullptr) {
            rootY += window->y;
            window = window->parent;
        }
        return rootY;
    }
};

struct WindowLock {
    std::mutex mutex;
   
   std::unique_lock<std::mutex> lock() {
       return std::unique_lock<std::mutex>(mutex);
   }
};

class WindowManager {
    private:
        std::unordered_map<int, std::unique_ptr<struct Window>> windows;
        Window *rootWindow = nullptr;
        std::string unviewableWMClass;
    
    public:
        WindowLock windowLock;
        
        WindowManager() {}
        void changeZOrder(int stackMode, Window *window, Window *sibling);
        void disableAllDescendants(Window *window);
        Window *getWindow(int id);
        void addWindow(int id, std::unique_ptr<struct Window> window);
        void deleteWindow(Window *window);
        Window* getRootWindow();
        void setRootWindow(Window *window); 
        void reparentWindow(Window *window, Window *parent);
        void setUnviewableWMClass(std::string className);
        std::string getUnviewableWMClass();
        std::unordered_map<int, std::unique_ptr<struct Window>>& getWindowTree();
};