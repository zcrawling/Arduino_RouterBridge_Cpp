//
// Created by sb on 1/30/26.
//

#ifndef ARDUINO_ROUTERBRIDGE_EVENT_H
#define ARDUINO_ROUTERBRIDGE_EVENT_H


#include <mutex>
#include <condition_variable>

class Event {
private:
    std::mutex mtx;
    std::condition_variable cv;
    bool flag = false;

public:
    void set() {
        std::lock_guard<std::mutex> lock(mtx);
        flag = true;
        cv.notify_all();
    }

    void wait() {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this] { return flag; });
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mtx);
        flag = false;
    }

    bool is_set() {
        std::lock_guard<std::mutex> lock(mtx);
        return flag;
    }
};


#endif //ARDUINO_ROUTERBRIDGE_EVENT_H