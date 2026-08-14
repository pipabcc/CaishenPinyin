#pragma once

#define SHURU_VERSION_MAJOR 0
#define SHURU_VERSION_MINOR 4
#define SHURU_VERSION_PATCH 5
#define SHURU_VERSION_STRING "0.4.5"

// 对外产品名（语言栏 / 注册表显示）
// 注意：此文件被 ShuruIme.rc 包含，不可使用非 ASCII 字符。
// 中文产品名统一从 RuntimeConfig::display_name 获取（默认 L"财神输入法"）。
#define SHURU_PRODUCT_NAME_A "Caishen IME"
#define SHURU_IME_FILENAME_W L"ShuruIme.dll"
