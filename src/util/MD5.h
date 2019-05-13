#pragma once

#include <cstdio>

namespace symutil {
    static constexpr int kMd5Length = 16;
    /*
     * Just a simple method for getting the signature
     * result must be == 16
     */
    void md5_signature(unsigned char *key, unsigned long length, unsigned char *result);

    void md5_stream(const char *file, unsigned char *result);

    std::string md5_stream_str(const char *file);
} /* symutil  */
