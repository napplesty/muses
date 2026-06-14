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

#include "muses/config.hpp"
#include "muses/net/poller.hpp"

#ifndef MUSES_NET_POLLER_FACTORY_HPP
#define MUSES_NET_POLLER_FACTORY_HPP

// IMPORTANT: include platform backends OUTSIDE namespace muses, otherwise the
// backend's own standard-library includes would be parsed inside muses::.
#ifdef MUSES_PLATFORM_KQUEUE
    #include "muses/net/kqueue_poller.hpp"
#elif defined(MUSES_PLATFORM_EPOLL)
    #include "muses/net/epoll_poller.hpp"
#endif

namespace muses {

// Factory implementation. Header-only; backend selected at compile time.
#ifdef MUSES_PLATFORM_KQUEUE
inline std::unique_ptr<Poller> make_poller() {
    return std::make_unique<KQueuePoller>();
}
#elif defined(MUSES_PLATFORM_EPOLL)
inline std::unique_ptr<Poller> make_poller() {
    return std::make_unique<EpollPoller>();
}
#endif

}  // namespace muses

#endif  // MUSES_NET_POLLER_FACTORY_HPP
