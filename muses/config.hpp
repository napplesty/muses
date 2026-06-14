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

// Common compile-time configuration and platform detection for muses.
// Include this first where platform macros are needed.

#ifndef MUSES_CONFIG_HPP
#define MUSES_CONFIG_HPP

// --- Platform detection ---------------------------------------------------

#if defined(__APPLE__) && defined(__MACH__)
    #define MUSES_PLATFORM_APPLE 1
    #define MUSES_PLATFORM_KQUEUE 1
#elif defined(__linux__)
    #define MUSES_PLATFORM_LINUX 1
    #define MUSES_PLATFORM_EPOLL 1
#else
    #error "muses only supports macOS (kqueue) and Linux (epoll)"
#endif

// --- Diagnostics hooks ----------------------------------------------------
// MUSES_ASSERT(expr, msg): used internally for invariant checks (e.g. memory
// pool magic validation). Resolves to a hard assert by default; tests may
// override the macro to observe failures without aborting.
#ifndef MUSES_ASSERT
    #include <cassert>
    #define MUSES_ASSERT(expr, msg) assert(((void)(msg), (expr)))
#endif

#endif // MUSES_CONFIG_HPP
