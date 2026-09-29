// Injects the client's mouse and keyboard into Windows with SendInput.
#pragma once

#include <cstdint>
#include <string>

#include "hyperlink/protocol.h"

namespace input {

// x, y are 0..65535 across the given monitor (desktop coordinates, physical pixels).
void mouseAbs(const hl::MonitorInfo& m, uint16_t x, uint16_t y);
void mouseRel(int dx, int dy);
void mouseButton(uint8_t button, bool down);
void scroll(int dy, int dx);
void key(uint16_t vk, bool down);
void text(const std::string& utf8);
// Lets go of every button and modifier this session pressed (disconnect mid-drag etc).
void releaseAll();

}  // namespace input
