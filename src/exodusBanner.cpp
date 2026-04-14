#include "exodusBanner.hpp"

namespace exodus
{

    std::string getBanner()
    {
        return "Exodus - EOP - V";
    } // LCOV_EXCL_LINE

    void printBanner(std::ostream& os)
    {
        // LCOV_EXCL_START
        os << getBanner() << '\n';
        // LCOV_EXCL_STOP
    }

    // LCOV_EXCL_START
    int runApp(std::ostream& os)
    {
        printBanner(os);
        return 0;
    }
    // LCOV_EXCL_STOP

} // namespace exodus
