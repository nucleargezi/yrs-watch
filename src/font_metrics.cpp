#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {
constexpr char gutter[] = " ";

struct Font {
    PangoLayout* layout = nullptr;
    int step = PANGO_SCALE;

    Font(const char* name, double scale) {
        g_autoptr(PangoFontMap) map = pango_cairo_font_map_new();
        PangoContext* context = pango_font_map_create_context(map);
        layout = pango_layout_new(context);
        g_object_unref(context);
        g_autoptr(PangoFontDescription) description = pango_font_description_from_string(name);
        double units = pango_font_description_get_size(description);
        bool absolute = pango_font_description_get_size_is_absolute(description);
        if (units <= 0) {
            units = 11.0 * PANGO_SCALE;
            absolute = false;
        }
        const double dpi = absolute ? 1 : 96.0 / 72.0;
        pango_font_description_set_absolute_size(description, std::round(units * dpi * scale));
        pango_layout_set_font_description(layout, description);
        step = std::max(1, marker_x("\t|"));
    }

    ~Font() { g_object_unref(layout); }

    // Pango snaps TAB to the same grid in each line and each native popup item.
    // Ask its shaper for the actual stop after this string, including the gutter.
    int stops(const std::string& text) {
        return std::max(1, marker_x(text + gutter + "\t|") / step);
    }

    int marker_x(const std::string& text) {
        pango_layout_set_text(layout, text.c_str(), static_cast<int>(text.size()));
        PangoRectangle position{};
        pango_layout_index_to_pos(layout, static_cast<int>(text.size()) - 1, &position);
        return position.x;
    }

    // Keep distinctive suffixes in unusually long model names without expanding the menu.
    std::string caption(const std::string& text) {
        if (stops(text) <= 14) return text;
        std::vector<size_t> boundaries{0};
        for (auto p = text.c_str(); *p;) {
            p = g_utf8_next_char(p);
            boundaries.push_back(static_cast<size_t>(p - text.c_str()));
        }
        size_t lo = 0, hi = boundaries.size() - 1;
        auto shortened = [&](size_t n) {
            size_t suffix = n / 3;
            return text.substr(0, boundaries[n - suffix]) + "…" +
                text.substr(boundaries[boundaries.size() - 1 - suffix]);
        };
        while (lo < hi) {
            size_t mid = (lo + hi + 1) / 2;
            if (stops(shortened(mid)) <= 14) lo = mid;
            else hi = mid - 1;
        }
        return shortened(lo);
    }
};

} // namespace

// The desktop service sends one batch of plain UTF-8 cells, one per line.
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    char* end = nullptr;
    const double scale = g_ascii_strtod(argv[2], &end);
    if (!end || *end || !std::isfinite(scale) || scale <= 0) return 2;
    Font font(argv[1], scale);
    std::string text;
    while (std::getline(std::cin, text)) {
        g_autofree gchar* valid = g_utf8_make_valid(text.c_str(), static_cast<gssize>(text.size()));
        const std::string clean(valid ? valid : "");
        std::cout << font.stops(clean) << '\t' << font.caption(clean) << '\n';
    }
    return std::cin.bad() || !std::cout ? 1 : 0;
}
