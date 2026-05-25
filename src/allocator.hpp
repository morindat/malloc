#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {
    void *malloc(std::size_t size);
    void  free(void *ptr);
    void *calloc(std::size_t nmemb, std::size_t size);
    void *realloc(void *ptr, std::size_t size);
}