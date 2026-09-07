#pragma once

#define SHURU_VERSION_MAJOR 2
#define SHURU_VERSION_MINOR 0
#define SHURU_VERSION_PATCH 2
#define SHURU_VERSION_STRING "2.0.2"

// Keep this header ASCII-only: it is also included by ShuruIme.rc.
// Localized product names come from RuntimeConfig::display_name.
#define SHURU_PRODUCT_NAME_A "Caishen IME"
#ifdef SHURU_IME_X86
#define SHURU_IME_FILENAME_A "ShuruIme32.dll"
#define SHURU_IME_FILENAME_W L"ShuruIme32.dll"
#else
#define SHURU_IME_FILENAME_A "ShuruIme.dll"
#define SHURU_IME_FILENAME_W L"ShuruIme.dll"
#endif
