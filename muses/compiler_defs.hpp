#pragma once

#ifndef MUSES_COMPILER_DEFS_HPP
#define MUSES_COMPILER_DEFS_HPP

#define muses_offset_of(TYPE, MEMBER) reinterpret_cast<char *>(&static_cast<TYPE *>(0)->MEMBER)
#define muses_container_of(TYPE, MEMBER, MEMBER_P) reinterpret_cast<TYPE *>(reinterpret_cast<char *>(MEMBER_P) - muses_offset_of(TYPE, MEMBER));

#endif