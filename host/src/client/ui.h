// Small Win32 helpers shared by the client windows.
#pragma once

#include <windows.h>

#include <string>

#include "devices.h"

namespace ui {

HFONT font(HWND forWindow);
int scale(HWND w, int px);  // 96-dpi pixels -> this window's pixels
// Modal dialog for a device's name, PIN and optional fallback address. Returns false if cancelled.
bool editDevice(HWND owner, SavedDevice& d, const std::wstring& title, bool askPinOnly = false);
void registerClasses(HINSTANCE inst);

}  // namespace ui
