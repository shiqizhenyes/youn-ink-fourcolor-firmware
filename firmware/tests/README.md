# Wi-Fi provisioning regression checks

From `firmware/`, run the host tests without ESP-IDF:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I main/components/78__esp-wifi-connect/include \
  tests/wifi_provisioning_test.cc -o /tmp/wifi_provisioning_test
/tmp/wifi_provisioning_test
```

To include JSON round-trip tests after dependencies have been downloaded:

```sh
cc -fsanitize=address,undefined \
  -c managed_components/espressif__cjson/cJSON/cJSON.c -o /tmp/wifi_cjson.o
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -DWIFI_TEST_JSON -I managed_components/espressif__cjson/cJSON \
  -I main/components/78__esp-wifi-connect/include \
  tests/wifi_provisioning_test.cc /tmp/wifi_cjson.o -o /tmp/wifi_provisioning_test
/tmp/wifi_provisioning_test
```

Coverage: full-width 32-byte SSIDs and 64-digit hexadecimal PSKs, UTF-8 names,
open networks, invalid credential lengths, fragmented HTTP bodies, receive
errors, and JSON escaping for quotes, backslashes and long names.

# Clock-aligned sleep steps

From `firmware/` (header-only, no ESP-IDF):

```sh
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I main/common tests/sleep_align_test.cc -o /tmp/sleep_align_test
/tmp/sleep_align_test
```

# InkSight weather/calendar checks

From `firmware/`, after dependencies have been downloaded:

```sh
cc -fsanitize=address,undefined \
  -c managed_components/espressif__cjson/cJSON/cJSON.c -o /tmp/ink_cjson.o
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I managed_components/espressif__cjson/cJSON -I main/common \
  tests/ink_data_test.cc main/common/ink_weather_data.cc \
  main/common/ink_calendar_data.cc main/common/gzip_util.cc \
  /tmp/ink_cjson.o -lz -o /tmp/ink_data_test
/tmp/ink_data_test
```

On the host `gzip_util.cc` uses zlib; on the device it uses the ROM's miniz
inflater. To also check the QWeather JWT (Ed25519 via libsodium, e.g.
`brew install libsodium`), add:

```sh
  -DINK_TEST_JWT -I "$(brew --prefix libsodium)/include" main/common/qweather_jwt.cc \
  -L "$(brew --prefix libsodium)/lib" -lsodium
```

Coverage: QWeather v7 now/7d/indices parsing from captured responses (numeric
strings, range wind scales such as "1-3", today's sunrise/sunset, missing or
unknown life indices, error payloads), the 4-day forecast with day labels taken
from the response
rather than the host clock, city lookup and its LocationID cache, gunzip of a
real server body (including size cap and truncation), the versioned NVS cache
round-trip and rejection of older caches, the spoken announcement text and its
Chinese number words, the daily announcement wake-up delay, wind and festival
tables, the lunar conversion, and 日柱/时辰/节气倒计时/高考天数 helpers. With `INK_TEST_JWT`, the Ed25519 signature is
checked against RFC 8032 test 1 and the JWT header/payload/signature layout is
verified, using placeholder IDs only.
`--dump-terms` and
`--dump-lunar` print the full tables for diffing against InkSight's
`backend/core/config.py` solar terms and the `zhdate` package.

# Wi-Fi hardware validation

Hardware validation remains required: provision on a 2.4 GHz network, reboot
and reconnect, retry after a wrong password, test a slow DHCP server, and
verify that credentials survive reboot. The device obtains the household
address and gateway via DHCP; the router need not use `192.168.0.1`.
The configuration hotspot uses `192.168.4.1`; same-subnet AP/STA conflicts
and WPA3/router-specific compatibility are not addressed by these tests.
