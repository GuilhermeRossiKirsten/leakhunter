#include <iostream>

#include "TestFramework.hpp"

int main() {
    std::cout << "LeakHunter unit tests\n\n";
    return lhtest::runAll();
}
