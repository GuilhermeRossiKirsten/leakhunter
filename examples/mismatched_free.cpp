/// Blocks released through the wrong entry point.
///
/// None of this leaks: every block is returned. All of it is undefined
/// behaviour, and all of it is the kind that keeps working for years -- these
/// types are trivially destructible and glibc's free() happens to accept what
/// operator new handed out. Add a destructor, or switch allocators, and the
/// same code starts corrupting the heap.
///
/// Expected: 0 leaks, 4 mismatched frees, exit code 1.
///
/// The `correctlyPaired()` call is load-bearing rather than decorative. The
/// detector refuses to report new/free pairings unless it has observed our
/// `operator delete` actually running, because a program that keeps its own
/// global operator delete (a static libstdc++, say) would otherwise have every
/// single `new` reported as freed with free(). One honest delete is what tells
/// the tool its interposition covers both halves of the pair.

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct Point {
    double x;
    double y;
};

/// `new` released with free(): free() never runs operator delete, and on a
/// non-trivial type it would also skip the destructor.
void newThenFree() {
    Point* point = new Point{1.0, 2.0};
    point->x += 1.0;
    std::free(point);  // should be `delete point`
}

/// `new[]` released with `delete`: only the first element is destroyed, and on
/// implementations that store an element count before the array the pointer
/// passed to operator delete is not even the one operator new[] returned.
void newArrayThenScalarDelete() {
    Point* points = new Point[64];
    points[0].x = 3.0;
    delete points;  // should be `delete[] points`
}

/// `new[]` released with free(): both mistakes at once.
void newArrayThenFree() {
    int* values = new int[256];
    values[0] = 7;
    std::free(values);  // should be `delete[] values`
}

/// malloc released with `delete`: the mirror image, and just as undefined.
void mallocThenDelete() {
    auto* raw = static_cast<Point*>(std::malloc(sizeof(Point)));
    if (raw == nullptr) {
        return;
    }
    std::memset(raw, 0, sizeof(Point));
    delete raw;  // should be `std::free(raw)`
}

/// The one that is right. See the note at the top of the file.
void correctlyPaired() {
    Point* point = new Point{0.0, 0.0};
    delete point;

    Point* points = new Point[8];
    delete[] points;

    void* block = std::malloc(128);
    std::free(block);
}

}  // namespace

int main() {
    correctlyPaired();

    newThenFree();
    newArrayThenScalarDelete();
    newArrayThenFree();
    mallocThenDelete();

    std::printf("every block was released -- through the wrong door\n");
    return 0;
}
