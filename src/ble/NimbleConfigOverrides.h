#pragma once
// Force-included into every translation unit (platformio.ini: -include).
//
// The Arduino core's precompiled sdkconfig.h enables NimBLE with all four
// roles and 3 connections, and NimBLE-Arduino reads sdkconfig.h before its
// own nimconfig.h defaults, so -D flags for these are silently overridden
// ("redefined" warning). sdkconfig.h is #pragma once: including it here
// first and then redefining makes these values the ones every later
// #include sees.
//
// BleHidClient (one BLE keyboard) is the only NimBLE user: central + observer
// roles and a single connection. Measured on the X3 before this change:
// NimBLEDevice::init() took ~57KB and left ~18KB free with a keyboard
// connected. Only the host (compiled from NimBLE-Arduino's sources) and the
// runtime controller config (ble_max_act = connections + broadcaster +
// observer, set in NimBLEDevice::init) use these; the precompiled controller
// is untouched. Preprocessor only: this is included into C files too.

#include "sdkconfig.h"

#undef CONFIG_BT_NIMBLE_MAX_CONNECTIONS
#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS 1

// Roles: nimconfig.h turns a role on unless it is already defined or its
// _DISABLED flag is set, and nimconfig_rename.h maps CONFIG_NIMBLE_* back,
// so drop both spellings and set the _DISABLED flag.
#undef CONFIG_BT_NIMBLE_ROLE_PERIPHERAL
#undef CONFIG_NIMBLE_ROLE_PERIPHERAL
#define CONFIG_BT_NIMBLE_ROLE_PERIPHERAL_DISABLED
#undef CONFIG_BT_NIMBLE_ROLE_BROADCASTER
#undef CONFIG_NIMBLE_ROLE_BROADCASTER
#define CONFIG_BT_NIMBLE_ROLE_BROADCASTER_DISABLED

// Host mbuf pool 2: 24 x 320B by default; a keyboard's traffic is tiny.
#undef CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT
#define CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT 12
