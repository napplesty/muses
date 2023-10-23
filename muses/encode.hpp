#pragma once

#ifndef _MUSES_SERIALIZE_HPP
#define _MUSES_SERIALIZE_HPP

#include <cstddef>
#include <vector>
#include <string>

namespace muses {

class Encoder {
public:
    virtual std::string to_string(const std::vector<std::byte> &byte_list) = 0;
    virtual std::vector<std::byte> from_string(const std::string &s) = 0;
    virtual std::vector<std::byte> encode(const std::vector<std::byte> &byte_list) = 0;
    virtual std::vector<std::byte> decode(const std::vector<std::byte> &byte_list) = 0;
};

class LZMAEncoder : public Encoder {

};

};
#endif