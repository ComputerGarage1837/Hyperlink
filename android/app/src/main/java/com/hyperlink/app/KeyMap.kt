package com.hyperlink.app

import android.view.KeyEvent

/** Android key codes to Windows virtual-key codes. */
object KeyMap {
    const val VK_BACK = 0x08
    const val VK_TAB = 0x09
    const val VK_RETURN = 0x0D
    const val VK_SHIFT = 0x10
    const val VK_CONTROL = 0x11
    const val VK_MENU = 0x12
    const val VK_ESCAPE = 0x1B
    const val VK_LEFT = 0x25
    const val VK_UP = 0x26
    const val VK_RIGHT = 0x27
    const val VK_DOWN = 0x28
    const val VK_DELETE = 0x2E
    const val VK_LWIN = 0x5B
    const val VK_HOME = 0x24
    const val VK_END = 0x23

    fun toVk(code: Int): Int {
        if (code in KeyEvent.KEYCODE_A..KeyEvent.KEYCODE_Z) return 0x41 + (code - KeyEvent.KEYCODE_A)
        if (code in KeyEvent.KEYCODE_0..KeyEvent.KEYCODE_9) return 0x30 + (code - KeyEvent.KEYCODE_0)
        if (code in KeyEvent.KEYCODE_F1..KeyEvent.KEYCODE_F12) return 0x70 + (code - KeyEvent.KEYCODE_F1)
        if (code in KeyEvent.KEYCODE_NUMPAD_0..KeyEvent.KEYCODE_NUMPAD_9) return 0x60 + (code - KeyEvent.KEYCODE_NUMPAD_0)
        return when (code) {
            KeyEvent.KEYCODE_DEL -> VK_BACK
            KeyEvent.KEYCODE_FORWARD_DEL -> VK_DELETE
            KeyEvent.KEYCODE_TAB -> VK_TAB
            KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER -> VK_RETURN
            KeyEvent.KEYCODE_ESCAPE -> VK_ESCAPE
            KeyEvent.KEYCODE_SPACE -> 0x20
            KeyEvent.KEYCODE_SHIFT_LEFT -> 0xA0
            KeyEvent.KEYCODE_SHIFT_RIGHT -> 0xA1
            KeyEvent.KEYCODE_CTRL_LEFT -> 0xA2
            KeyEvent.KEYCODE_CTRL_RIGHT -> 0xA3
            KeyEvent.KEYCODE_ALT_LEFT -> 0xA4
            KeyEvent.KEYCODE_ALT_RIGHT -> 0xA5
            KeyEvent.KEYCODE_META_LEFT -> VK_LWIN
            KeyEvent.KEYCODE_META_RIGHT -> 0x5C
            KeyEvent.KEYCODE_CAPS_LOCK -> 0x14
            KeyEvent.KEYCODE_DPAD_LEFT -> VK_LEFT
            KeyEvent.KEYCODE_DPAD_UP -> VK_UP
            KeyEvent.KEYCODE_DPAD_RIGHT -> VK_RIGHT
            KeyEvent.KEYCODE_DPAD_DOWN -> VK_DOWN
            KeyEvent.KEYCODE_MOVE_HOME -> VK_HOME
            KeyEvent.KEYCODE_MOVE_END -> VK_END
            KeyEvent.KEYCODE_PAGE_UP -> 0x21
            KeyEvent.KEYCODE_PAGE_DOWN -> 0x22
            KeyEvent.KEYCODE_INSERT -> 0x2D
            KeyEvent.KEYCODE_SYSRQ -> 0x2C
            KeyEvent.KEYCODE_SCROLL_LOCK -> 0x91
            KeyEvent.KEYCODE_BREAK -> 0x13
            KeyEvent.KEYCODE_NUM_LOCK -> 0x90
            KeyEvent.KEYCODE_MENU -> 0x5D
            KeyEvent.KEYCODE_SEMICOLON -> 0xBA
            KeyEvent.KEYCODE_EQUALS -> 0xBB
            KeyEvent.KEYCODE_COMMA -> 0xBC
            KeyEvent.KEYCODE_MINUS -> 0xBD
            KeyEvent.KEYCODE_PERIOD -> 0xBE
            KeyEvent.KEYCODE_SLASH -> 0xBF
            KeyEvent.KEYCODE_GRAVE -> 0xC0
            KeyEvent.KEYCODE_LEFT_BRACKET -> 0xDB
            KeyEvent.KEYCODE_BACKSLASH -> 0xDC
            KeyEvent.KEYCODE_RIGHT_BRACKET -> 0xDD
            KeyEvent.KEYCODE_APOSTROPHE -> 0xDE
            KeyEvent.KEYCODE_NUMPAD_MULTIPLY -> 0x6A
            KeyEvent.KEYCODE_NUMPAD_ADD -> 0x6B
            KeyEvent.KEYCODE_NUMPAD_SUBTRACT -> 0x6D
            KeyEvent.KEYCODE_NUMPAD_DOT -> 0x6E
            KeyEvent.KEYCODE_NUMPAD_DIVIDE -> 0x6F
            KeyEvent.KEYCODE_VOLUME_UP -> 0xAF
            KeyEvent.KEYCODE_VOLUME_DOWN -> 0xAE
            KeyEvent.KEYCODE_VOLUME_MUTE -> 0xAD
            KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE -> 0xB3
            KeyEvent.KEYCODE_MEDIA_NEXT -> 0xB0
            KeyEvent.KEYCODE_MEDIA_PREVIOUS -> 0xB1
            else -> 0
        }
    }
}
