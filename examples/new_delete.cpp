/// C++ allocation paths: operator new, operator new[], and the ones that are
/// correctly matched with delete.
///
/// Expected: 2 leaks -- one `new Widget` and one `new int[256]`. The
/// std::unique_ptr and the balanced new/delete pair must not be reported.

#include <cstdio>
#include <memory>

namespace {

struct Widget {
    int values[16];
    double weight;
};

Widget* leakOneWidget() {
    return new Widget{};
}

int* leakAnArray() {
    return new int[256];
}

void balancedNewDelete() {
    for (int i = 0; i < 1000; ++i) {
        auto* widget = new Widget{};
        delete widget;
    }
}

void smartPointerIsFine() {
    auto widget = std::make_unique<Widget>();
    widget->weight = 1.0;
}

}  // namespace

int main() {
    Widget* leakedWidget = leakOneWidget();
    int* leakedArray = leakAnArray();

    balancedNewDelete();
    smartPointerIsFine();

    std::printf("leaked a Widget at %p and an array at %p\n",
                static_cast<void*>(leakedWidget), static_cast<void*>(leakedArray));

    // No delete / delete[] on purpose.
    return 0;
}
