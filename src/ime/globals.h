#pragma once

#include <Windows.h>
#include <msctf.h>

#include <atomic>
#include <string>

namespace shuru {

extern HINSTANCE g_module;
extern std::atomic<long> g_dll_ref;
extern CRITICAL_SECTION g_cs;

void DllAddRef();
void DllRelease();

// 词库目录：优先 ProgramData 下 current 指向的版本包，兼容 DLL 旁旧目录。
std::wstring GetLexiconDirectory();

} // namespace shuru
