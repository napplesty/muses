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
#include <mutex>
#include <iostream>
#include <tuple>
#include <map>
#include <sstream>
#include <functional>
#include <future>
#include <condition_variable>
#include <set>

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "muses/logging.hpp"

namespace muses {

// Network Driver Interfaces
class ListenHandler {
public:
    virtual bool init_listener() {return false;};
    virtual int get_listener() {return -1;};
};

template <class context_class>
class ConnectionHandler {
public:
    virtual bool init(int listen_fd, std::function<bool(context_class *, int)>) {return false;};
};

// To realize the functions.
class TCPListener : public ListenHandler{
public:
    TCPListener (std::string ip, unsigned short port):
    ip(ip), port(port),
    running(false), listen_fd(-1) {}

    ~TCPListener() {close(listen_fd);}

    bool init_listener() {
        struct sockaddr_in address{};
        int opt = 1;
        int addrlen = sizeof(address);
        
        if ((listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == 0) {
            MUSES_ERROR("tcp inaddress setup failed");
            return false;
        }

        // if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR|SO_REUSEPORT, &opt, sizeof(opt))) {
        //     MUSES_ERROR("tcp setsockopt failed");
        //     return false;
        // }

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
}; // namespace muses

#ifdef __APPLE__

#include <sys/_types/_socklen_t.h>
#include <sys/_types/_ssize_t.h>
#include <sys/file.h>
#include <sys/event.h>
#include "muses/queue.hpp"
#include "muses/thread_pool.hpp"
#include "muses/memory_pool.hpp"

namespace muses {

template <class context_class>
class KqueueConnectionHandler : public ConnectionHandler<context_class> {
public:
    KqueueConnectionHandler(int max_events, int max_threads)
    : max_events(max_events),
    max_threads(max_threads),
    manage_threads(new ThreadPool(2)),
    process_threads(new ThreadPool(max_threads)),
    inited(false),
    context_memory_pool(sizeof(context_class)+1, max_events) {context_memory_pool.initialize();}

    ~KqueueConnectionHandler() {
        inited = false;
        delete manage_threads;
        delete process_threads;
    }

    bool init(int listen_fd, std::function<bool(context_class *, int)> process_func) {
        if(inited) {
            return true;
        }
        this->listen_fd = listen_fd;
        this->process_func = process_func;

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
        manage_threads->enqueue(KqueueConnectionHandler<context_class>::internal_manage_connected_fd, this);
        manage_threads->enqueue(KqueueConnectionHandler<context_class>::internal_manage_recycle_resource, this);
        return true;
    }

private:
    static void internal_manage_connected_fd(KqueueConnectionHandler<context_class> *cls_instance) {
        while(true) {
            if (cls_instance->inited == false) {
                return;
            }

            int event_count = kevent(cls_instance->kqueue_fd, nullptr, 0, cls_instance->events, cls_instance->max_events, nullptr);
            if (event_count == -1) {
                MUSES_ERROR("Failed to wait for events");
                break;
            }
            for( int i = 0; i < event_count; i++) {
                int fd = cls_instance->events[i].ident;
                int filter = cls_instance->events[i].filter;
                if (fd == cls_instance->listen_fd && filter == EVFILT_READ) {
                    sockaddr_in client_address{};
                    socklen_t client_address_length = sizeof(client_address);

                    int client_fd = accept(cls_instance->listen_fd, reinterpret_cast<struct sockaddr*>(&client_address), &client_address_length);
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
                    if (kevent(cls_instance->kqueue_fd, &client_event, 1, nullptr, 0, 0) == -1) {
                        MUSES_ERROR("Failed to add client socket to kqueue");
                        close(client_fd);
                        continue;
                    }
                    cls_instance->context_map[client_fd] = static_cast<context_class *>(cls_instance->context_memory_pool.allocate());
                } else {
                    // avoid process fd that is already in the process queue
                    if (cls_instance->occuping_fds.count(fd) == 0) {
                        cls_instance->occuping_fds.insert(fd);
                        cls_instance->results_queue.push(std::move(std::tuple<int, std::future<bool> >(fd, cls_instance->process_threads->enqueue(cls_instance->process_func, cls_instance->context_map[fd], fd))));
                    }
                }
            }
        }
    }
    static void internal_manage_recycle_resource(KqueueConnectionHandler<context_class> *cls_instance) {
        while(true) {
            if (cls_instance->inited == false) {
                return;
            }

            std::tuple<int, std::future<bool> > a_result;
            cls_instance->results_queue.wait_and_pop(a_result);
            if (std::get<1>(a_result).get() == false) {
                cls_instance->context_memory_pool.deallocate(cls_instance->context_map[std::get<0>(a_result)]);
                for(auto pos = cls_instance->context_map.begin(); pos != cls_instance->context_map.end(); pos++) {
                    if (pos->first == std::get<0>(a_result)) {
                        cls_instance->context_map.erase(pos);
                        break;
                    }
                }
                struct kevent delete_connected_event{};
                EV_SET(&delete_connected_event, std::get<0>(a_result), EVFILT_READ, EV_DELETE, 0, 0, nullptr);
                if (kevent(cls_instance->kqueue_fd, &delete_connected_event, 1, cls_instance->events, 1, 0) == -1) {
                    MUSES_ERROR("Delete kqueue event failed");
                }
                close(std::get<0>(a_result));
            }
            cls_instance->occuping_fds.erase(std::get<0>(a_result));
        }
    }

public:
    bool inited;

    int max_events;
    int max_threads;
    int listen_fd;
    int kqueue_fd;
    struct kevent *events;

    ThreadPool *manage_threads;
    ThreadPool *process_threads;

    std::function<bool(context_class *, int)> process_func;
    ThreadSafeQueue<std::tuple<int, std::future<bool> > > results_queue;
    MemoryPool context_memory_pool;
    std::set<int> occuping_fds;
    std::map<int, context_class *> context_map;
};
}; // namespace muses
#endif

#endif