// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/base/ime/input_method_win.h"

#include "base/basictypes.h"
#include "base/debug/stack_trace.h"
#include "base/logging.h"
#include "base/string_util.h"
#include "base/win/metro.h"
#include "ui/base/events/event.h"
#include "ui/base/events/event_constants.h"
#include "ui/base/events/event_utils.h"
#include "ui/base/ime/composition_text.h"
#include "ui/base/ime/input_method.h"
#include "ui/base/ime/text_input_client.h"
#include "ui/base/ime/win/tsf_bridge.h"
#include "ui/base/keycodes/keyboard_codes.h"
#include "ui/base/win/hwnd_util.h"

// Extra number of chars before and after selection (or composition) range which
// is returned to IME for improving conversion accuracy.
static const size_t kExtraNumberOfChars = 20;

namespace ui {

InputMethodWin::InputMethodWin(internal::InputMethodDelegate* delegate,
                               HWND hwnd)
    : hwnd_(hwnd),
      active_(false),
      direction_(base::i18n::UNKNOWN_DIRECTION),
      pending_requested_direction_(base::i18n::UNKNOWN_DIRECTION),
      enabled_(false) {
  SetDelegate(delegate);
}

InputMethodWin::~InputMethodWin() {
  ime_input_.DisableIME(hwnd_);
}

void InputMethodWin::Init(bool focused) {
  // Gets the initial input locale and text direction information.
  OnInputLangChange(0, 0);

  InputMethodBase::Init(focused);
}

void InputMethodWin::OnFocus() {
  InputMethodBase::OnFocus();
  UpdateIMEState();
}

void InputMethodWin::OnBlur() {
  ConfirmCompositionText();
  InputMethodBase::OnBlur();
}

bool InputMethodWin::DispatchKeyEvent(
    const base::NativeEvent& native_key_event) {
  if (native_key_event.message == WM_CHAR) {
    BOOL handled;
    OnChar(native_key_event.message, native_key_event.wParam,
           native_key_event.lParam, &handled);
    return !!handled;  // Don't send WM_CHAR for post event processing.
  }
  // Handles ctrl-shift key to change text direction and layout alignment.
  if (ui::ImeInput::IsRTLKeyboardLayoutInstalled() && !IsTextInputTypeNone()) {
    // TODO: shouldn't need to generate a KeyEvent here.
    const ui::KeyEvent key(native_key_event,
                           native_key_event.message == WM_CHAR);
    ui::KeyboardCode code = key.key_code();
    if (key.type() == ui::ET_KEY_PRESSED) {
      if (code == ui::VKEY_SHIFT) {
        base::i18n::TextDirection dir;
        if (ui::ImeInput::IsCtrlShiftPressed(&dir))
          pending_requested_direction_ = dir;
      } else if (code != ui::VKEY_CONTROL) {
        pending_requested_direction_ = base::i18n::UNKNOWN_DIRECTION;
      }
    } else if (key.type() == ui::ET_KEY_RELEASED &&
               (code == ui::VKEY_SHIFT || code == ui::VKEY_CONTROL) &&
               pending_requested_direction_ != base::i18n::UNKNOWN_DIRECTION) {
      GetTextInputClient()->ChangeTextDirectionAndLayoutAlignment(
          pending_requested_direction_);
      pending_requested_direction_ = base::i18n::UNKNOWN_DIRECTION;
    }
  }

  return DispatchKeyEventPostIME(native_key_event);
}

bool InputMethodWin::DispatchFabricatedKeyEvent(const ui::KeyEvent& event) {
  // TODO(ananta)
  // Support IMEs and RTL layout in Windows 8 metro Ash. The code below won't
  // work with IMEs.
  // Bug: https://code.google.com/p/chromium/issues/detail?id=164964
  if (event.is_char()) {
    if (GetTextInputClient()) {
      GetTextInputClient()->InsertChar(event.key_code(),
                                       ui::GetModifiersFromKeyState());
      return true;
    }
  }
  return DispatchFabricatedKeyEventPostIME(event.type(),
                                           event.key_code(),
                                           event.flags());
}

void InputMethodWin::OnTextInputTypeChanged(const TextInputClient* client) {
  if (base::win::IsTSFAwareRequired()) {
    UpdateIMEState();
    ui::TSFBridge::GetInstance()->OnTextInputTypeChanged(
        const_cast<TextInputClient*>(client));
    return;
  }
  if (IsTextInputClientFocused(client)) {
    ime_input_.CancelIME(hwnd_);
    UpdateIMEState();
  }
  InputMethodBase::OnTextInputTypeChanged(client);
}

void InputMethodWin::OnCaretBoundsChanged(const TextInputClient* client) {
  if (!enabled_ || !IsTextInputClientFocused(client))
    return;
  // The current text input type should not be NONE if |client| is focused.
  DCHECK(!IsTextInputTypeNone());
  gfx::Rect screen_bounds(GetTextInputClient()->GetCaretBounds());
  // TODO(ime): see comment in TextInputClient::GetCaretBounds(), this
  // conversion shouldn't be necessary.
  RECT r;
  GetClientRect(hwnd_, &r);
  POINT window_point = { screen_bounds.x(), screen_bounds.y() };
  ScreenToClient(hwnd_, &window_point);
  ime_input_.UpdateCaretRect(
      hwnd_,
      gfx::Rect(gfx::Point(window_point.x, window_point.y),
                screen_bounds.size()));
}

void InputMethodWin::CancelComposition(const TextInputClient* client) {
  if (enabled_ && IsTextInputClientFocused(client))
    ime_input_.CancelIME(hwnd_);
}

std::string InputMethodWin::GetInputLocale() {
  return locale_;
}

base::i18n::TextDirection InputMethodWin::GetInputTextDirection() {
  return direction_;
}

bool InputMethodWin::IsActive() {
  return active_;
}

void InputMethodWin::SetFocusedTextInputClient(TextInputClient* client) {
  if (base::win::IsTSFAwareRequired()) {
    if (client) {
      ui::TSFBridge::GetInstance()->SetFocusedClient(hwnd_, client);
    } else {
      ui::TSFBridge::GetInstance()->RemoveFocusedClient(
          ui::TSFBridge::GetInstance()->GetFocusedTextInputClient());
      return;
    }
  }
  InputMethodBase::SetFocusedTextInputClient(client);
}

TextInputClient* InputMethodWin::GetTextInputClient() const {
  if (base::win::IsTSFAwareRequired())
    return ui::TSFBridge::GetInstance()->GetFocusedTextInputClient();
  return InputMethodBase::GetTextInputClient();
}

LRESULT InputMethodWin::OnImeMessages(UINT message,
                                      WPARAM w_param,
                                      LPARAM l_param,
                                      BOOL* handled) {
  // Don't do traditional IME in Text services mode.
  if (base::win::IsTSFAwareRequired() &&
      message != WM_CHAR &&
      message != WM_SYSCHAR &&
      message != WM_DEADCHAR &&
      message != WM_SYSDEADCHAR &&
      message != WM_IME_SETCONTEXT) {
    return false;
  }
  LRESULT result = 0;
  switch (message) {
    case WM_IME_SETCONTEXT:
      result = OnImeSetContext(message, w_param, l_param, handled);
      break;
    case WM_IME_STARTCOMPOSITION:
      result = OnImeStartComposition(message, w_param, l_param, handled);
      break;
    case WM_IME_COMPOSITION:
      result = OnImeComposition(message, w_param, l_param, handled);
      break;
    case WM_IME_ENDCOMPOSITION:
      result = OnImeEndComposition(message, w_param, l_param, handled);
      break;
    case WM_IME_REQUEST:
      result = OnImeRequest(message, w_param, l_param, handled);
      break;
    case WM_CHAR:
    case WM_SYSCHAR:
      result = OnChar(message, w_param, l_param, handled);
      break;
    case WM_DEADCHAR:
    case WM_SYSDEADCHAR:
      result = OnDeadChar(message, w_param, l_param, handled);
      break;
    default:
      NOTREACHED() << "Unknown IME message:" << message;
      break;
  }
  return result;
}

void InputMethodWin::OnInputLangChange(DWORD character_set,
                                       HKL input_language_id) {
  active_ = ime_input_.SetInputLanguage();
  locale_ = ime_input_.GetInputLanguageName();
  direction_ = ime_input_.GetTextDirection();
  OnInputMethodChanged();
}

void InputMethodWin::OnWillChangeFocusedClient(TextInputClient* focused_before,
                                               TextInputClient* focused) {
  ConfirmCompositionText();
}

void InputMethodWin::OnDidChangeFocusedClient(TextInputClient* focused_before,
                                              TextInputClient* focused) {
  // Force to update the input type since client's TextInputStateChanged()
  // function might not be called if text input types before the client loses
  // focus and after it acquires focus again are the same.
  OnTextInputTypeChanged(focused);

  UpdateIMEState();

  // Force to update caret bounds, in case the client thinks that the caret
  // bounds has not changed.
  OnCaretBoundsChanged(focused);
}

LRESULT InputMethodWin::OnImeSetContext(UINT message,
                                        WPARAM wparam,
                                        LPARAM lparam,
                                        BOOL* handled) {
  active_ = (wparam == TRUE);
  if (active_)
    ime_input_.CreateImeWindow(hwnd_);

  OnInputMethodChanged();
  return ime_input_.SetImeWindowStyle(hwnd_, message, wparam, lparam, handled);
}

LRESULT InputMethodWin::OnImeStartComposition(UINT message,
                                              WPARAM wparam,
                                              LPARAM lparam,
                                              BOOL* handled) {
  // We have to prevent WTL from calling ::DefWindowProc() because the function
  // calls ::ImmSetCompositionWindow() and ::ImmSetCandidateWindow() to
  // over-write the position of IME windows.
  *handled = TRUE;

  if (IsTextInputTypeNone())
    return 0;

  // Reset the composition status and create IME windows.
  ime_input_.CreateImeWindow(hwnd_);
  ime_input_.ResetComposition(hwnd_);
  return 0;
}

LRESULT InputMethodWin::OnImeComposition(UINT message,
                                         WPARAM wparam,
                                         LPARAM lparam,
                                         BOOL* handled) {
  // We have to prevent WTL from calling ::DefWindowProc() because we do not
  // want for the IMM (Input Method Manager) to send WM_IME_CHAR messages.
  *handled = TRUE;

  if (IsTextInputTypeNone())
    return 0;

  // At first, update the position of the IME window.
  ime_input_.UpdateImeWindow(hwnd_);

  // Retrieve the result string and its attributes of the ongoing composition
  // and send it to a renderer process.
  ui::CompositionText composition;
  if (ime_input_.GetResult(hwnd_, lparam, &composition.text)) {
    GetTextInputClient()->InsertText(composition.text);
    ime_input_.ResetComposition(hwnd_);
    // Fall though and try reading the composition string.
    // Japanese IMEs send a message containing both GCS_RESULTSTR and
    // GCS_COMPSTR, which means an ongoing composition has been finished
    // by the start of another composition.
  }
  // Retrieve the composition string and its attributes of the ongoing
  // composition and send it to a renderer process.
  if (ime_input_.GetComposition(hwnd_, lparam, &composition))
    GetTextInputClient()->SetCompositionText(composition);

  return 0;
}

LRESULT InputMethodWin::OnImeEndComposition(UINT message,
                                            WPARAM wparam,
                                            LPARAM lparam,
                                            BOOL* handled) {
  // Let WTL call ::DefWindowProc() and release its resources.
  *handled = FALSE;

  if (IsTextInputTypeNone())
    return 0;

  if (GetTextInputClient()->HasCompositionText())
    GetTextInputClient()->ClearCompositionText();

  ime_input_.ResetComposition(hwnd_);
  ime_input_.DestroyImeWindow(hwnd_);
  return 0;
}

LRESULT InputMethodWin::OnImeRequest(UINT message,
                                     WPARAM wparam,
                                     LPARAM lparam,
                                     BOOL* handled) {
  *handled = FALSE;

  // Should not receive WM_IME_REQUEST message, if IME is disabled.
  const ui::TextInputType type = GetTextInputType();
  if (type == ui::TEXT_INPUT_TYPE_NONE ||
      type == ui::TEXT_INPUT_TYPE_PASSWORD) {
    return 0;
  }

  switch (wparam) {
    case IMR_RECONVERTSTRING:
      *handled = TRUE;
      return OnReconvertString(reinterpret_cast<RECONVERTSTRING*>(lparam));
    case IMR_DOCUMENTFEED:
      *handled = TRUE;
      return OnDocumentFeed(reinterpret_cast<RECONVERTSTRING*>(lparam));
    default:
      return 0;
  }
}

LRESULT InputMethodWin::OnChar(UINT message,
                               WPARAM wparam,
                               LPARAM lparam,
                               BOOL* handled) {
  *handled = TRUE;

  // We need to send character events to the focused text input client event if
  // its text input type is ui::TEXT_INPUT_TYPE_NONE.
  if (GetTextInputClient()) {
    GetTextInputClient()->InsertChar(static_cast<char16>(wparam),
                                     ui::GetModifiersFromKeyState());
  }

  // Explicitly show the system menu at a good location on [Alt]+[Space].
  // Note: Setting |handled| to FALSE for DefWindowProc triggering of the system
  //       menu causes undesirable titlebar artifacts in the classic theme.
  if (message == WM_SYSCHAR && wparam == VK_SPACE)
    ui::ShowSystemMenu(hwnd_);

  return 0;
}

LRESULT InputMethodWin::OnDeadChar(UINT message,
                                   WPARAM wparam,
                                   LPARAM lparam,
                                   BOOL* handled) {
  *handled = TRUE;

  if (IsTextInputTypeNone())
    return 0;

  if (!GetTextInputClient())
    return 0;

  // Shows the dead character as a composition text, so that the user can know
  // what dead key was pressed.
  ui::CompositionText composition;
  composition.text.assign(1, static_cast<char16>(wparam));
  composition.selection = ui::Range(0, 1);
  composition.underlines.push_back(
      ui::CompositionUnderline(0, 1, SK_ColorBLACK, false));
  GetTextInputClient()->SetCompositionText(composition);
  return 0;
}

LRESULT InputMethodWin::OnDocumentFeed(RECONVERTSTRING* reconv) {
  ui::TextInputClient* client = GetTextInputClient();
  if (!client)
    return 0;

  ui::Range text_range;
  if (!client->GetTextRange(&text_range) || text_range.is_empty())
    return 0;

  bool result = false;
  ui::Range target_range;
  if (client->HasCompositionText())
    result = client->GetCompositionTextRange(&target_range);

  if (!result || target_range.is_empty()) {
    if (!client->GetSelectionRange(&target_range) ||
        !target_range.IsValid()) {
      return 0;
    }
  }

  if (!text_range.Contains(target_range))
    return 0;

  if (target_range.GetMin() - text_range.start() > kExtraNumberOfChars)
    text_range.set_start(target_range.GetMin() - kExtraNumberOfChars);

  if (text_range.end() - target_range.GetMax() > kExtraNumberOfChars)
    text_range.set_end(target_range.GetMax() + kExtraNumberOfChars);

  size_t len = text_range.length();
  size_t need_size = sizeof(RECONVERTSTRING) + len * sizeof(WCHAR);

  if (!reconv)
    return need_size;

  if (reconv->dwSize < need_size)
    return 0;

  string16 text;
  if (!GetTextInputClient()->GetTextFromRange(text_range, &text))
    return 0;
  DCHECK_EQ(text_range.length(), text.length());

  reconv->dwVersion = 0;
  reconv->dwStrLen = len;
  reconv->dwStrOffset = sizeof(RECONVERTSTRING);
  reconv->dwCompStrLen =
      client->HasCompositionText() ? target_range.length() : 0;
  reconv->dwCompStrOffset =
      (target_range.GetMin() - text_range.start()) * sizeof(WCHAR);
  reconv->dwTargetStrLen = target_range.length();
  reconv->dwTargetStrOffset = reconv->dwCompStrOffset;

  memcpy((char*)reconv + sizeof(RECONVERTSTRING),
         text.c_str(), len * sizeof(WCHAR));

  // According to Microsft API document, IMR_RECONVERTSTRING and
  // IMR_DOCUMENTFEED should return reconv, but some applications return
  // need_size.
  return reinterpret_cast<LRESULT>(reconv);
}

LRESULT InputMethodWin::OnReconvertString(RECONVERTSTRING* reconv) {
  ui::TextInputClient* client = GetTextInputClient();
  if (!client)
    return 0;

  // If there is a composition string already, we don't allow reconversion.
  if (client->HasCompositionText())
    return 0;

  ui::Range text_range;
  if (!client->GetTextRange(&text_range) || text_range.is_empty())
    return 0;

  ui::Range selection_range;
  if (!client->GetSelectionRange(&selection_range) ||
      selection_range.is_empty()) {
    return 0;
  }

  DCHECK(text_range.Contains(selection_range));

  size_t len = selection_range.length();
  size_t need_size = sizeof(RECONVERTSTRING) + len * sizeof(WCHAR);

  if (!reconv)
    return need_size;

  if (reconv->dwSize < need_size)
    return 0;

  // TODO(penghuang): Return some extra context to help improve IME's
  // reconversion accuracy.
  string16 text;
  if (!GetTextInputClient()->GetTextFromRange(selection_range, &text))
    return 0;
  DCHECK_EQ(selection_range.length(), text.length());

  reconv->dwVersion = 0;
  reconv->dwStrLen = len;
  reconv->dwStrOffset = sizeof(RECONVERTSTRING);
  reconv->dwCompStrLen = len;
  reconv->dwCompStrOffset = 0;
  reconv->dwTargetStrLen = len;
  reconv->dwTargetStrOffset = 0;

  memcpy(reinterpret_cast<char*>(reconv) + sizeof(RECONVERTSTRING),
         text.c_str(), len * sizeof(WCHAR));

  // According to Microsft API document, IMR_RECONVERTSTRING and
  // IMR_DOCUMENTFEED should return reconv, but some applications return
  // need_size.
  return reinterpret_cast<LRESULT>(reconv);
}

void InputMethodWin::ConfirmCompositionText() {
  if (!IsTextInputTypeNone()) {
    ime_input_.CleanupComposition(hwnd_);
    // Though above line should confirm the client's composition text by sending
    // a result text to us, in case the input method and the client are in
    // inconsistent states, we check the client's composition state again.
    if (GetTextInputClient()->HasCompositionText())
      GetTextInputClient()->ConfirmCompositionText();
  }
}

void InputMethodWin::UpdateIMEState() {
  // Use switch here in case we are going to add more text input types.
  // We disable input method in password field.
  switch (GetTextInputType()) {
    case ui::TEXT_INPUT_TYPE_NONE:
    case ui::TEXT_INPUT_TYPE_PASSWORD:
      ime_input_.DisableIME(hwnd_);
      enabled_ = false;
      break;
    default:
      ime_input_.EnableIME(hwnd_);
      enabled_ = true;
      break;
  }
}

}  // namespace ui
