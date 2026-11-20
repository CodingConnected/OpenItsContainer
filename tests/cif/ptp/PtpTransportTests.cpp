// ABI smoke test for the PTP C driver contract. Verifies the extern "C" symbols
// the hosted controller calls are present, link, and behave gracefully when no
// PTP links are configured (activeOptions() is empty in a test binary, so the
// lazily-constructed PtpManager holds zero links). This exercises the real
// singleton + extern "C" adapter path that PtpLinkSetTests cannot reach.

#include <gtest/gtest.h>

// The driver contract, exactly as the controller's ptpcom.c declares it.
extern "C" {
void init_COM_IBER(int portnr);
void init_COM_UBER(int portnr);
char lees_char_COM_IBER(int portnr);
int  bezet_COM_IBER(int portnr);
void schrijf_char_COM_UBER(int portnr, char c);
}

namespace {
constexpr char EOB = static_cast<char>(-1);
}

TEST(PtpTransportAbi, UnconfiguredLinksAreGraceful) {
    // No configured links -> every port reports empty / not busy, and writes are
    // dropped without crashing. The controller sees a permanently-down link.
    for (int portnr = 1; portnr <= 4; ++portnr) {
        EXPECT_EQ(bezet_COM_IBER(portnr), 0);
        EXPECT_EQ(lees_char_COM_IBER(portnr), EOB);
        EXPECT_NO_FATAL_FAILURE(schrijf_char_COM_UBER(portnr, 'x'));
        EXPECT_NO_FATAL_FAILURE(init_COM_IBER(portnr));
        EXPECT_NO_FATAL_FAILURE(init_COM_UBER(portnr));
    }
}

TEST(PtpTransportAbi, OutOfRangePortnrIsGraceful) {
    EXPECT_EQ(bezet_COM_IBER(0), 0);
    EXPECT_EQ(lees_char_COM_IBER(0), EOB);
    EXPECT_EQ(bezet_COM_IBER(99), 0);
    EXPECT_EQ(lees_char_COM_IBER(99), EOB);
    EXPECT_NO_FATAL_FAILURE(schrijf_char_COM_UBER(99, 'x'));
}
