#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <glm/glm.hpp>

float lerp(float a, float b, float t);
float unlerp(float val, float a, float b);
float rescale(float val, float fromA, float fromB, float toA, float toB);

float approach(float current, float target, float delta);

// I would use boost's lexical cast, if I had another reason to use boost
template <typename T = long long>
std::optional<T> parseInt(const std::string& str, int base = 10)
{
    static constexpr auto min = std::numeric_limits<T>::min();
    static constexpr auto max = std::numeric_limits<T>::max();
    try {
        size_t pos = 0;
        const auto val = std::stoll(str, &pos, base);
        if (pos < str.size())
            return std::nullopt;
        if (val < min || val > max)
            return std::nullopt;
        return static_cast<T>(val);
    } catch (const std::exception& exc) {
        return std::nullopt;
    }
}

std::optional<float> parseFloat(const std::string& str);

std::string hexStream(const uint8_t* data, size_t len);

std::string toLower(const std::string& str);

std::vector<std::string> split(const std::string& str);

template <typename T>
T safeNormalize(const T& vec)
{
    const auto len = glm::length(vec) + 1e-5f;
    return vec / len;
}

template <typename... Args>
void println(Args&&... args)
{
    fmt::print(std::forward<Args>(args)...);
    std::putc('\n', stdout);
    std::fflush(stdout);
}

template <typename... Args>
void println(std::FILE* f, Args&&... args)
{
    fmt::print(f, std::forward<Args>(args)...);
    std::putc('\n', f);
    std::fflush(f);
}

template <typename... Args>
void printErr(Args&&... args)
{
    println(stderr, std::forward<Args>(args)...);
}

template <typename T>
T sign(T val)
{
    return (val > T {}) - (val < T {});
}
