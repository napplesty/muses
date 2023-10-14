// MIT License

// Copyright (c) 2023 nastyapple

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#ifndef _NET_DRIVER_HPP
#define _NET_DRIVER_HPP

#include <string>
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sstream>
#include <functional>
#include <future>
#include "muses/logging.hpp"
#include "muses/queue.hpp"
#include "muses/thread_pool.hpp"
#include "muses/memory_pool.hpp"
#include <condition_variable>
#include <sys/_types/_socklen_t.h>
#include <sys/_types/_ssize_t.h>
#include <tuple>
#include <map>

#ifdef __APPLE__
#include <sys/event.h>
#endif

namespace muses {

// Network Driver Interfaces
class ListenHandler {
public:
    virtual bool init_listener() = 0;
    virtual int get_listener() = 0;
};

template <class context_class>
class ConnectionHandler {
public:
    virtual bool init_muxer(int listen_fd) = 0;
    virtual void init_message_protocol(std::function<bool (context_class *, int connected_fd)>) = 0;
};

// To realize the functions.
class TCPListener : public ListenHandler{
public:
    TCPListener (std::string ip, unsigned short port):
    ip(ip), port(port),
    running(false), listen_fd(-1) {}

    ~TCPListener() {shutdown(listen_fd, 2);}

    bool init_listener() {
        struct sockaddr_in address{};
        int opt = 1;
        int addrlen = sizeof(address);
        
        if ((listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == 0) {
            MUSES_ERROR("tcp inaddress setup failed");
            return false;
        }

        if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR|SO_REUSEPORT, &opt, sizeof(opt))) {
            MUSES_ERROR("tcp setsockopt failed");
            return false;
        }

        address.sin_family = AF_INET;
        address.sin_addr.s_addr = inet_addr(ip.c_str());
        address.sin_port = htons(port);

        if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
            MUSES_ERROR("tcp bind failed");
            return false;
        }

        if (listen(listen_fd, SOMAXCONN) < 0) {
            MUSES_ERROR("socket listen failed");
            return false;
        }

        std::stringstream ss;
        ss << "tcp listen to " << ip << ':' << port;
        MUSES_INFO(ss.str());
        ss.str("");
        ss.clear();

        running = true;
        return true;
    }

    int get_listener() {
        if (!running) {
            init_listener();
        }
        return listen_fd;
    }
private:
    bool running;
    int listen_fd;
    unsigned short port;
    std::string ip;
};

#ifdef __APPLE__

template <class context_class>
class KqueueConnectionHandler : public ConnectionHandler<context_class> {
public:
    KqueueConnectionHandler(int max_events, int max_thread)
    :max_events(max_events),
    inited(false), running(true),
    thread_handle_listen(&KqueueConnectionHandler<context_class>::add_connected_fd_to_queue, this),
    thread_recycle(&KqueueConnectionHandler<context_class>::recycle, this),
    max_thread(max_thread),
    context_memory_pool(sizeof(context_class), max_events) {}
    ~KqueueConnectionHandler() {
        if (inited) {
            running = false;
            delete events;
            cv_handle_listen.notify_all();
            thread_handle_listen.join();
        }
    }

    bool init_muxer(int listen_fd) {
        if(inited) {
            return true;
        }
        kqueue_fd = kqueue();
        if (kqueue_fd == -1) {
            MUSES_ERROR("Fail to create kqueue");
            return false;
        }

        struct kevent listen_event{};
        events = (new struct kevent[max_events]);

        EV_SET(&listen_event, listen_fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
        if (kevent(kqueue_fd, &listen_event, 1, nullptr, 0, nullptr) == -1) {
            MUSES_ERROR("Fail to add listen socket to kqueue");
            return false;
        }
        MUSES_INFO("Server start. Waiting for connections...");
        inited = true;
        return inited;
    }

    void init_message_protocol(std::function<bool (context_class *, int connected_fd)> handle_func) {
        this->handle_func = handle_func;
    };

private:
    // Let a thread to retrieve the connected file descriptor
    // from the kqueue.
    void add_connected_fd_to_queue() {
        ThreadPool thread_pool(this->max_thread);
        while(true) {
            std::unique_lock<std::mutex> lock(mutex_handle_listen);
            cv_handle_listen.wait(lock, [this]{return this->inited | !this->running;});
            if(!this->running) {
                break;
            }
            int event_count = kevent(kqueue_fd, nullptr, 0, events, max_events, nullptr);
            if (event_count == -1) {
                MUSES_ERROR("Failed to wait for events");
                break;
            }
            for (int i = 0; i < event_count; i++) {
                int fd = events[i].ident;
                int filter = events[i].filter;
                if (fd == listen_fd && filter == EVFILT_READ) {
                    sockaddr_in client_address{};
                    socklen_t client_address_length = sizeof(client_address);

                    int client_fd = accept(listen_fd, reinterpret_cast<struct sockaddr*>(&client_address), &client_address_length);
                    if(client_fd == -1) {
                        MUSES_ERROR("Fail to accept new connection");
                    }

                    std::stringstream ss;
                    ss << "New Connection from " << inet_ntoa(client_address.sin_addr);
                    MUSES_INFO(ss.str());
                    ss.str("");
                    ss.clear();

                    struct kevent client_event{};
                    EV_SET(&client_event, client_fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
                    if (kevent(kqueue_fd, &client_event, 1, nullptr, 0, 0) == -1) {
                        MUSES_ERROR("Failed to add client socket to kqueue");
                        shutdown(client_fd, 2);
                        // close(client_fd); // I do not know why my mac does not have this api
                        continue;
                    }
                    this->context_map[client_fd] = static_cast<context_class *>(context_memory_pool.allocate());
                } else if (filter == EVFILT_READ) {
                    results_queue.push(std::move(std::tuple<int, std::future<bool> >(fd, thread_pool.enqueue(this->handle_func, context_map[fd]))));
                }
            }
        }
    }

    void recycle() {
        while (true) {
            std::unique_lock<std::mutex> lock(mutex_recycle);
            cv_recycle.wait(lock, [this]{return !this->results_queue.empty();});
            std::tuple<int, std::future<bool> > a_result;
            if (std::get<1>(a_result).get() == false) {
                context_memory_pool.deallocate(context_map[std::get<0>(a_result)]);
                for(auto pos = context_map.begin(); pos != context_map.end(); pos++) {
                    if (pos->first == std::get<0>(a_result)) {
                        context_map.erase(pos);
                        break;
                    }
                }
            }
        }
    }

private:
    bool inited;
    bool running;

    int max_events;
    int listen_fd;
    int kqueue_fd;
    struct kevent *events;
    std::mutex mutex_handle_listen;
    std::condition_variable cv_handle_listen;
    std::thread thread_handle_listen;

    std::function<bool (context_class &, int connected_fd)> handle_func;
    std::map<int, context_class *> context_map;
    MemoryPool context_memory_pool;

    int max_thread;
    ThreadSafeQueue<std::tuple<int, std::future<bool> > > results_queue;
    std::mutex mutex_recycle;
    std::condition_variable cv_recycle;
    std::thread thread_recycle;
};

#endif

};

#endif