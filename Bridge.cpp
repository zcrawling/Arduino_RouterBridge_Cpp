// SPDX-FileCopyrightText: Copyright (C) ARDUINO SRL (http://www.arduino.cc)
// SPDX-License-Identifier: MPL-2.0
// NOTE: Ported from bridge.py in https://github.com/arduino/app-bricks-py
// NOTE: REQUIRED: msgpack-cpp lib: https://github.com/msgpack/msgpack-c/tree/cpp_master
#include <msgpack.hpp>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>
#include <future>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>



constexpr double RECONNECT_DELAY_SEC = 3.0;
constexpr uint8_t ROUTE_ALREADY_EXISTS_ERR = 0x05;
constexpr uint8_t MALFORMED_CALL_ERR      = 0xFD;
constexpr uint8_t FUNCTION_NOT_FOUND_ERR  = 0xFE;
constexpr uint8_t GENERIC_ERR             = 0xFF;

struct Logger { //TODO() implement Logger(for debug)
    std::string name;
    explicit Logger(const std::string& n) : name(n) {}
    template <typename... Args>
    void info(Args&&...){}
    template <typename... Args>
    void warning(Args&&...){}
    template <typename... Args>
    void error(Args&&...){}
};

static Logger logger("Bridge");

class ClientServer {
public:
    using MsgPackObject = msgpack::object;
    using Params        = std::vector<msgpack::object_handle>;
    using Handler       = std::function<msgpack::object_handle(const std::vector<msgpack::object>&)>;

    static ClientServer& instance(const std::string& address = "unix:///var/run/arduino-router.sock") {
        static ClientServer inst(address);
        return inst;
    }

    // Notify: fire-and-forget
    void notify(const std::string& method_name, const std::vector<msgpack::object>& params) {
        msgpack::sbuffer sbuf;
        msgpack::packer<msgpack::sbuffer> pk(&sbuf);
        pk.pack_array(3);
        pk.pack(2);
        pk.pack(method_name);
        pk.pack_array(params.size());
        for (auto& p : params)
            pk.pack(p);

        try {
            sendBytes(sbuf.data(), sbuf.size());
        } catch (...) {
        }
    }

    // Call: request/response
    msgpack::object_handle call(const std::string& method_name,
                                const std::vector<msgpack::object>& params,
                                int timeout_sec = 10)
    {
        uint32_t msgid = incrementMsgId();
        msgpack::sbuffer sbuf;
        msgpack::packer<msgpack::sbuffer> pk(&sbuf);
        pk.pack_array(4);
        pk.pack(0);
        pk.pack(msgid);
        pk.pack(method_name);
        pk.pack_array(params.size());
        for (auto& p : params)
            pk.pack(p);

        std::promise<std::variant<msgpack::object_handle, std::pair<int, std::string>>> prom;
        auto fut = prom.get_future();
        {
            std::lock_guard<std::mutex> lock(callbacks_mutex_);
            callbacks_[msgid] = std::move(prom);
        }
        try {
            sendBytes(sbuf.data(), sbuf.size());
        } catch (...) {
            std::lock_guard<std::mutex> lock(callbacks_mutex_);
            callbacks_.erase(msgid);
            throw std::runtime_error("Failed to send request");
        }

        if (timeout_sec <= 0) {
            auto v = fut.get();
            return handleCallResult(method_name, msgid, v);
        } else {
            if (fut.wait_for(std::chrono::seconds(timeout_sec)) == std::future_status::timeout) {
                // timeout
                {
                    std::lock_guard<std::mutex> lock(callbacks_mutex_);
                    auto it = callbacks_.find(msgid);
                    if (it != callbacks_.end()) {
                        callbacks_.erase(it);
                    }
                }
                throw std::runtime_error("Request '" + method_name + "' timed out");
            }
            auto v = fut.get();
            return handleCallResult(method_name, msgid, v);
        }
    }


    //provide: register RPC call from server
    void provide(const std::string& method_name, Handler handler) {
        if (!handler) {
            throw std::invalid_argument("Handler must be callable");
        }
        {
            msgpack::zone z;
            std::vector<msgpack::object> params;
            params.emplace_back(method_name, z);
            call("$/register", params, 10);
        }

        std::lock_guard<std::mutex> lock(handlers_mutex_);
        handlers_[method_name] = std::move(handler);
    }

    void unprovide(const std::string& method_name) {
        {
            std::lock_guard<std::mutex> lock(handlers_mutex_);
            if (!handlers_.contains(method_name))
                return;
        }

        {
            msgpack::zone z;
            std::vector<msgpack::object> params;
            params.emplace_back(method_name, z);
            call("$/unregister", params, 10);
        }

        std::lock_guard<std::mutex> lock2(handlers_mutex_);
        handlers_.erase(method_name);
    }

private:
    ClientServer(const std::string& address)
        : next_msgid_(0),
          socket_fd_(-1),
          stop_flag_(false)
    {
        parseAddress(address);
        connectLoopOnce();
        read_thread_ = std::thread(&ClientServer::connManager, this);
    }

    ~ClientServer() {
        stop_flag_.store(true);
        if (read_thread_.joinable())
            read_thread_.join();
        if (socket_fd_ >= 0)
            close(socket_fd_);
    }

    ClientServer(const ClientServer&) = delete;
    ClientServer& operator=(const ClientServer&) = delete;

    enum class SocketType { Unix, Tcp };

    void parseAddress(const std::string& address) {
        if (address.rfind("unix://", 0) == 0) {
            socket_type_ = SocketType::Unix;
            peer_unix_path_ = address.substr(7);
        } else if (address.rfind("tcp://", 0) == 0) {
            socket_type_ = SocketType::Tcp;
            auto rest = address.substr(6);
            auto colon = rest.find(':');
            if (colon == std::string::npos)
                throw std::runtime_error("Invalid tcp address");
            peer_tcp_host_ = rest.substr(0, colon);
            peer_tcp_port_ = std::stoi(rest.substr(colon + 1));
        } else {
            throw std::runtime_error("Unsupported address scheme");
        }
    }

    void connManager() {
        while (!stop_flag_.load()) {
            connectLoopOnce();
            readLoop();
            std::this_thread::sleep_for(std::chrono::milliseconds(
                static_cast<int>(RECONNECT_DELAY_SEC * 1000)));
        }
    }

    void connectLoopOnce() {
        if (isConnected())
            return;

        if (socket_fd_ >= 0) {
            close(socket_fd_);
            socket_fd_ = -1;
        }

        while (!stop_flag_.load() && !isConnected()) {
            try {
                if (socket_type_ == SocketType::Unix) {
                    socket_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
                    if (socket_fd_ < 0)
                        throw std::runtime_error("socket() failed");
                    sockaddr_un addr{};
                    addr.sun_family = AF_UNIX;
                    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", peer_unix_path_.c_str());
                    if (::connect(socket_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
                        throw std::runtime_error("connect() failed");
                } else {
                    socket_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
                    if (socket_fd_ < 0)
                        throw std::runtime_error("socket() failed");
                    sockaddr_in addr{};
                    addr.sin_family = AF_INET;
                    addr.sin_port   = htons(peer_tcp_port_);
                    if (::inet_pton(AF_INET, peer_tcp_host_.c_str(), &addr.sin_addr) <= 0)
                        throw std::runtime_error("inet_pton() failed");
                    if (::connect(socket_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
                        throw std::runtime_error("connect() failed");
                }

                {
                    std::lock_guard<std::mutex> lock(handlers_mutex_);
                    if (!handlers_.empty()) {
                        std::thread([this]{
                            std::lock_guard<std::mutex> lh(handlers_mutex_);
                            for (auto& [name, _] : handlers_) {
                                try {
                                    msgpack::zone z;
                                    std::vector<msgpack::object> params;
                                    params.emplace_back(name, z);
                                    call("$/register", params, 10);
                                } catch (...) {
                                    logger.error("Failed to re-register method");
                                }
                            }
                        }).detach();
                    }
                }
                break;
            } catch (...) {
                if (socket_fd_ >= 0) {
                    close(socket_fd_);
                    socket_fd_ = -1;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    static_cast<int>(RECONNECT_DELAY_SEC * 1000)));
            }
        }
    }

    bool isConnected() const {
        return socket_fd_ >= 0;
    }

    void readLoop() {
        if (socket_fd_ < 0)
            return;

        msgpack::unpacker unp;
        try {
            while (!stop_flag_.load()) {
                char buf[4096];
                ssize_t n = ::recv(socket_fd_, buf, sizeof(buf), 0);
                if (n <= 0) {
                    break;
                }
                unp.reserve_buffer(n);
                std::memcpy(unp.buffer(), buf, n);
                unp.buffer_consumed(n);

                msgpack::object_handle oh;
                while (unp.next(oh)) {
                    handleMsg(oh.get());
                }
            }
        } catch (...) {
        }
        failPendingCallbacks(std::runtime_error("Connection lost"));
        if (socket_fd_ >= 0) {
            close(socket_fd_);
            socket_fd_ = -1;
        }
    }

    void handleMsg(const msgpack::object& obj) {
        if (obj.type != msgpack::type::ARRAY)
            return;
        auto arr = obj.via.array;
        if (arr.size == 0)
            return;

        int msg_type;
        arr.ptr[0].convert(msg_type);

        try {
            if (msg_type == 0) { // request [0, msgid, method, params]
                if (arr.size != 4)
                    throw std::runtime_error("Invalid request length");
                uint32_t msgid;
                std::string method_name;
                arr.ptr[1].convert(msgid);
                arr.ptr[2].convert(method_name);

                std::vector<msgpack::object> params;
                if (arr.ptr[3].type == msgpack::type::ARRAY) {
                    auto p_arr = arr.ptr[3].via.array;
                    for (uint32_t i = 0; i < p_arr.size; ++i)
                        params.push_back(p_arr.ptr[i]);
                }

                Handler handler;
                {
                    std::lock_guard<std::mutex> lock(handlers_mutex_);
                    auto it = handlers_.find(method_name);
                    if (it != handlers_.end())
                        handler = it->second;
                }

                if (handler) {
                    try {
                        auto result = handler(params);
                        sendResponse(msgid, nullptr, &result);
                    } catch (const std::exception& e) {
                        logger.error("Handler error");
                        sendResponse(msgid, &e, nullptr);
                    }
                } else {
                    std::string msg = "Method not found: " + method_name;
                    std::runtime_error e(msg);
                    sendResponse(msgid, &e, nullptr);
                }
            } else if (msg_type == 1) { // response [1, msgid, error, result]
                if (arr.size != 4)
                    throw std::runtime_error("Invalid response length");
                uint32_t msgid;
                arr.ptr[1].convert(msgid);

                msgpack::object err_obj = arr.ptr[2];
                msgpack::object res_obj = arr.ptr[3];

                std::optional<std::pair<int, std::string>> err_pair;
                if (!err_obj.is_nil()) {
                    if (err_obj.type == msgpack::type::ARRAY &&
                        err_obj.via.array.size >= 2) {
                        int code;
                        std::string msg;
                        err_obj.via.array.ptr[0].convert(code);
                        err_obj.via.array.ptr[1].convert(msg);
                        err_pair = std::make_pair(code, msg);
                    } else {
                        throw std::runtime_error("Invalid error format");
                    }
                }

                std::promise<std::variant<msgpack::object_handle, std::pair<int, std::string>>> prom;
                bool has_cb = false;
                {
                    std::lock_guard<std::mutex> lock(callbacks_mutex_);
                    auto it = callbacks_.find(msgid);
                    if (it != callbacks_.end()) {
                        prom = std::move(it->second);
                        callbacks_.erase(it);
                        has_cb = true;
                    }
                }

                if (has_cb) {
                    if (!err_pair || err_pair->first == ROUTE_ALREADY_EXISTS_ERR) {
                        msgpack::sbuffer sbuf;
                        msgpack::pack(sbuf, res_obj);
                        msgpack::object_handle oh = msgpack::unpack(sbuf.data(), sbuf.size());
                        prom.set_value(std::move(oh));
                    } else {
                        prom.set_value(*err_pair);
                    }
                }
            } else if (msg_type == 2) { // notification [2, method, params]
                if (arr.size != 3)
                    throw std::runtime_error("Invalid notification length");

                std::string method_name;
                arr.ptr[1].convert(method_name);

                std::vector<msgpack::object> params;
                if (arr.ptr[2].type == msgpack::type::ARRAY) {
                    auto p_arr = arr.ptr[2].via.array;
                    for (uint32_t i = 0; i < p_arr.size; ++i)
                        params.push_back(p_arr.ptr[i]);
                }

                Handler handler;
                {
                    std::lock_guard<std::mutex> lock(handlers_mutex_);
                    auto it = handlers_.find(method_name);
                    if (it != handlers_.end())
                        handler = it->second;
                }

                if (handler) {
                    try {
                        handler(params);
                    } catch (...) {
                        logger.error("Notification handler error");
                    }
                }
            }
        } catch (...) {
            logger.error("Error while handling msg");
        }
    }

    void failPendingCallbacks(const std::exception& e) {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        for (auto& [_, prom] : callbacks_) {
            prom.set_value(std::make_pair((int)GENERIC_ERR, e.what()));
        }
        callbacks_.clear();
    }

    void sendResponse(uint32_t msgid,
                      const std::exception* error,
                      const msgpack::object_handle* response)
    {
        msgpack::sbuffer sbuf;
        msgpack::packer<msgpack::sbuffer> pk(&sbuf);
        pk.pack_array(4);
        pk.pack(1); // response
        pk.pack(msgid);

        if (error) {
            int code = GENERIC_ERR;
            std::string msg = error->what();
            pk.pack_array(2);
            pk.pack(code);
            pk.pack(msg);
        } else {
            pk.pack_nil();
        }

        if (response) {
            pk.pack(response->get());
        } else {
            pk.pack_nil();
        }

        try {
            sendBytes(sbuf.data(), sbuf.size());
        } catch (...) {
        }
    }

    void sendBytes(const void* data, size_t len) {
        if (!isConnected())
            throw std::runtime_error("Not connected");

        size_t sent_total = 0;
        while (sent_total < len) {
            ssize_t n = ::send(socket_fd_,
                               static_cast<const char*>(data) + sent_total,
                               len - sent_total, 0);
            if (n <= 0)
                throw std::runtime_error("send() failed");
            sent_total += n;
        }
    }

    uint32_t incrementMsgId() {
        std::lock_guard<std::mutex> lock(msgid_mutex_);
        next_msgid_ = (next_msgid_ + 1) % (1u << 32);
        return next_msgid_;
    }

    msgpack::object_handle handleCallResult(const std::string& method_name,
                                            uint32_t msgid,
                                            const std::variant<msgpack::object_handle,
                                                               std::pair<int, std::string>>& v)
    {
        if (std::holds_alternative<msgpack::object_handle>(v)) {
            auto& oh = std::get<msgpack::object_handle>(v);
            msgpack::zone z;  // 새 zone 생성
            return msgpack::object_handle(oh.get(), std::make_unique<msgpack::zone>(std::move(z)));
        }
        auto [code, msg] = std::get<std::pair<int, std::string>>(v);
        throw std::runtime_error("Request '" + method_name + "' failed: " + msg);
    }

private:
    // state
    uint32_t next_msgid_;
    std::mutex msgid_mutex_;

    std::map<uint32_t, std::promise<std::variant<msgpack::object_handle,
                                                 std::pair<int, std::string>>>> callbacks_;
    std::mutex callbacks_mutex_;

    std::map<std::string, Handler> handlers_;
    std::mutex handlers_mutex_;

    SocketType socket_type_;
    std::string peer_unix_path_;
    std::string peer_tcp_host_;
    int peer_tcp_port_ = 0;

    int socket_fd_;
    std::thread read_thread_;
    std::atomic<bool> stop_flag_;
};

//C- style wrapper class
namespace Bridge {

inline void notify(const std::string& method_name,
                   const std::vector<msgpack::object>& params) {
    ClientServer::instance().notify(method_name, params);
}

inline msgpack::object_handle call(const std::string& method_name,
                                   const std::vector<msgpack::object>& params,
                                   int timeout_sec = 10) {
    return ClientServer::instance().call(method_name, params, timeout_sec);
}

inline void provide(const std::string& method_name,
                    ClientServer::Handler handler) {
    ClientServer::instance().provide(method_name, std::move(handler));
}

inline void unprovide(const std::string& method_name) {
    ClientServer::instance().unprovide(method_name);
}

}



//example:

// int main() {
//     msgpack::zone zone;
//     std::vector<msgpack::object> vec;
//
//     int val_int = 10;
//     vec.push_back(msgpack::object(val_int, zone));
//
//
//     std::string val_str = "123";
//     vec.push_back(msgpack::object(val_str, zone));
//     Bridge::notify("sts",vec);
//
// }
