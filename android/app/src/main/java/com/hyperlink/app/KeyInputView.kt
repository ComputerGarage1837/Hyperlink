package com.hyperlink.app

import android.content.Context
import android.text.InputType
import android.view.KeyEvent
import android.view.View
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager

/**
 * Invisible view that receives the on-screen keyboard. Typed text goes to the host as text;
 * Backspace, Enter and friends as keys.
 */
class KeyInputView(ctx: Context, private val onText: (String) -> Unit, private val onKey: (Int) -> Unit) : View(ctx) {
    init {
        isFocusable = true
        isFocusableInTouchMode = true
    }

    override fun onCheckIsTextEditor() = true

    override fun onCreateInputConnection(out: EditorInfo): InputConnection {
        // No suggestions or autocorrect: every key press should reach the PC right away.
        out.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD or
            InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        out.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI or EditorInfo.IME_FLAG_NO_FULLSCREEN or
            EditorInfo.IME_ACTION_NONE
        return object : BaseInputConnection(this, false) {
            override fun commitText(text: CharSequence, newCursorPosition: Int): Boolean {
                if (text.isNotEmpty()) onText(text.toString())
                return true
            }

            override fun setComposingText(text: CharSequence, newCursorPosition: Int): Boolean {
                // Keyboards that insist on composing: treat each composition as final.
                if (text.isNotEmpty()) onText(text.toString())
                finishComposingText()
                return true
            }

            override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
                repeat(beforeLength.coerceAtMost(64).coerceAtLeast(if (beforeLength > 0) 1 else 0)) {
                    onKey(KeyEvent.KEYCODE_DEL)
                }
                repeat(afterLength.coerceAtMost(64)) { onKey(KeyEvent.KEYCODE_FORWARD_DEL) }
                return true
            }

            override fun sendKeyEvent(event: KeyEvent): Boolean {
                if (event.action == KeyEvent.ACTION_DOWN) onKey(event.keyCode)
                return true
            }

            override fun performEditorAction(actionCode: Int): Boolean {
                onKey(KeyEvent.KEYCODE_ENTER)
                return true
            }
        }
    }

    var keyboardShown = false
        private set

    fun toggleKeyboard() {
        val imm = context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        if (keyboardShown) {
            imm.hideSoftInputFromWindow(windowToken, 0)
            keyboardShown = false
        } else {
            requestFocus()
            imm.showSoftInput(this, InputMethodManager.SHOW_FORCED)
            keyboardShown = true
        }
    }
}
