// Host test for SecondsUntilSteppedAlign; see tests/README.md.
#include "weather_prefs.h"

#include <cassert>
#include <cstdio>

namespace {

int At(int hour, int minute, int second = 0) {
    return hour * 3600 + minute * 60 + second;
}

void Expect(int now, int interval_min, int expected, const char* label) {
    const int got = ink::SecondsUntilSteppedAlign(now, interval_min);
    if (got != expected) {
        std::fprintf(stderr, "FAIL %s: interval=%d now=%d got=%d expected=%d\n",
                     label, interval_min, now, got, expected);
        assert(got == expected);
    }
}

}  // namespace

int main() {
    // 30 min: 07:20 -> 07:30
    Expect(At(7, 20), 30, 10 * 60, "07:20/30 -> 07:30");
    // 60 min: 06:31 -> 07:00
    Expect(At(6, 31), 60, 29 * 60, "06:31/60 -> 07:00");
    // 60 min on the mark: 07:00 -> 08:00
    Expect(At(7, 0), 60, 60 * 60, "07:00/60 -> 08:00");
    // 120 min: 07:20 -> 08:00
    Expect(At(7, 20), 120, 40 * 60, "07:20/120 -> 08:00");
    // 10 min, 07:18: next :30 is 12 min away, so sleep a full 10 min
    Expect(At(7, 18), 10, 10 * 60, "07:18/10 -> +10min");
    // 10 min, 07:28: 2 min to :30, snap
    Expect(At(7, 28), 10, 2 * 60, "07:28/10 -> 07:30");
    Expect(At(7, 0), 0, -1, "interval 0");
    std::printf("sleep_align_test ok\n");
    return 0;
}
