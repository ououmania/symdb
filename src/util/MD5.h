#pragma once

#include <string>
#include <cstdio>

namespace symutil {

static constexpr int kMd5Length = 16;
static constexpr int kMd5StrLength = kMd5Length * 2 + 1;  // include nil
/*
 * Just a simple method for getting the signature
 * result must be == 16
 */
void md5_signature(unsigned char *key, unsigned long length,
                   unsigned char *result);

void md5_file(const char *file, unsigned char *result);

void md5_file_str(const char *file, char *result);

std::string md5_file_str(const char *file);

}  // namespace symutil
