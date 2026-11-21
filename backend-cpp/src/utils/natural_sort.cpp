#include "utils/natural_sort.hpp"

#include <cctype>

namespace koma {

bool natural_less(const std::string& a, const std::string& b) {
    size_t ia = 0;
    size_t ib = 0;
    const size_t na = a.size();
    const size_t nb = b.size();

    while (ia < na && ib < nb) {
        const unsigned char ca = static_cast<unsigned char>(a[ia]);
        const unsigned char cb = static_cast<unsigned char>(b[ib]);

        if (std::isdigit(ca) && std::isdigit(cb)) {
            // Skip leading zeros while tracking run length
            size_t za = ia;
            while (za < na && a[za] == '0') ++za;
            size_t zb = ib;
            while (zb < nb && b[zb] == '0') ++zb;

            size_t ea = za;
            while (ea < na && std::isdigit(static_cast<unsigned char>(a[ea]))) ++ea;
            size_t eb = zb;
            while (eb < nb && std::isdigit(static_cast<unsigned char>(b[eb]))) ++eb;

            const size_t len_a = ea - za;
            const size_t len_b = eb - zb;
            if (len_a != len_b) {
                return len_a < len_b;
            }
            for (size_t k = 0; k < len_a; ++k) {
                if (a[za + k] != b[zb + k]) {
                    return a[za + k] < b[zb + k];
                }
            }
            // Equal numeric value: prefer fewer leading zeros if different
            if ((za - ia) != (zb - ib)) {
                return (za - ia) < (zb - ib);
            }
            ia = ea;
            ib = eb;
        } else {
            const char la = static_cast<char>(std::tolower(ca));
            const char lb = static_cast<char>(std::tolower(cb));
            if (la != lb) {
                return la < lb;
            }
            ++ia;
            ++ib;
        }
    }
    return ia == na && ib < nb;
}

} // namespace koma
