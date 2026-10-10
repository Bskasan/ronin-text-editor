// vec.cpp: a small 2D vector type and a templated sum.

#include <cmath>
#include <cstdio>
#include <vector>

namespace geo {

/* A plain value type: no constructors needed,
   the members have default values. */
struct Vec2 {
    float x = 0.0f, y = 0.0f;

    constexpr Vec2 operator+(Vec2 o) const {
        return {x + o.x, y + o.y};
    }
    float length() const { return std::sqrt(x * x + y * y); }
};

template <typename T>
T sum(const std::vector<T> &items) {
    T total{};
    for (const auto &item : items) {
        total = total + item;
    }
    return total;
}

} // namespace geo

int main() {
    std::vector<geo::Vec2> path = {{3, 4}, {1.5f, -2}, {0, 1e-3f}};
    auto end = geo::sum(path);
    const char *label = R"(end of "path")";
    if (end.length() > 0x10) {
        std::printf("far away: %s\n", label);
    } else {
        std::printf("%s = (%g, %g)\n", label, end.x, end.y);
    }
    return label == nullptr ? 1 : 0;
}
