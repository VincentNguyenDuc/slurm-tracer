#include <omp.h>

#include <iostream>

#include "pair.hpp"
#include "spdlog/spdlog.h"

int main(int, char**) {
    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("[%H:%M:%S %z] [thread %t] %v");

    const Pair<int> intPair{3, 5};
    const Pair<double> doublePair{2.5, 4.5};

#pragma omp parallel
    {
        spdlog::info("Hello, World");
        spdlog::info("Integer addition: {}", intPair.sum());
        spdlog::info("Double addition: {}", doublePair.sum());
        spdlog::info("Max of intPair: {}", intPair.max());
        spdlog::info("Max of doublePair: {}", doublePair.max());
        spdlog::info("First of intPair: {}", intPair.first());
        spdlog::info("Second of intPair: {}", intPair.second());
    }

    return 0;
}
