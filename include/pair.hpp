#ifndef PAIR_HPP
#define PAIR_HPP

#include "spdlog/spdlog.h"

template <typename T>
class Pair {
private:
    T a_, b_;

public:
    Pair(const T& a, const T& b)
        : a_{a}
        , b_{b} {
        spdlog::debug("Pair constructed: ({}, {})", a, b);
        spdlog::debug("Type of T: {}", typeid(T).name());
    }

    const T& first() const { return a_; }
    T& first() { return a_; }

    const T& second() const { return b_; }
    T& second() { return b_; }

    T sum() const { return a_ + b_; }

    T max() const { return (a_ > b_) ? a_ : b_; }

    bool is_equal(const Pair<T>& other) const { return a_ == other.a_ && b_ == other.b_; }
};

#endif // PAIR_HPP