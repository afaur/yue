// Copyright 2016 Cheng Zhao. All rights reserved.
// Copyright 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by the license that can be found in the
// LICENSE file.

#include "nativeui/win/window_win.h"

#include <memory>

#include "nativeui/accelerator.h"
#include "nativeui/accelerator_manager.h"
#include "nativeui/events/event.h"
#include "nativeui/events/win/event_win.h"
#include "nativeui/gfx/geometry/insets.h"
#include "nativeui/gfx/geometry/rect_conversions.h"
#include "nativeui/gfx/win/double_buffer.h"
#include "nativeui/gfx/win/painter_win.h"
#include "nativeui/gfx/win/screen_win.h"
#include "nativeui/menu_bar.h"
#include "nativeui/win/menu_base_win.h"
#include "nativeui/win/subwin_view.h"
#include "nativeui/win/util/hwnd_util.h"
#include "third_party/yoga/yoga/Yoga.h"

namespace nu {

namespace {

// Default window style for frameless window.
const DWORD kWindowDefaultFramelessStyle =
    WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;

// Width of the resize handle.
const int kResizeBorderWidth = 5;

// At the window corners the resize area is not actually bigger, but the 16
// pixels at the end of the top and bottom edges trigger diagonal resizing.
const int kResizeCornerWidth = 16;

// Convert between window and client areas.
Rect ContentToWindowBounds(Win32Window* window, bool has_menu_bar,
                           const Rect& bounds) {
  RECT rect = bounds.ToRECT();
  AdjustWindowRectEx(&rect, window->window_style(), has_menu_bar,
                     window->window_ex_style());
  return Rect(rect);
}

inline bool IsShiftPressed() {
  return (::GetKeyState(VK_SHIFT) & 0x8000) == 0x8000;
}

}  // namespace

WindowImpl::WindowImpl(const Window::Options& options, Window* delegate)
    : Win32Window(L"", NULL,
                  options.frame ? kWindowDefaultStyle
                                : kWindowDefaultFramelessStyle,
                  options.transparent ? WS_EX_LAYERED : 0),
      delegate_(delegate),
      scale_factor_(GetScaleFactorForHWND(hwnd())) {
  if (!options.frame) {
    // First nccalcszie (during CreateWindow) for captioned windows is
    // deliberately ignored so force a second one here to get the right
    // non-client set up.
    ::SetWindowPos(hwnd(), NULL, 0, 0, 0, 0,
                   SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                   SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
  }
  if (options.transparent) {
    // Change default background color to transparent.
    background_color_ = Color(0, 0, 0, 0);
  }
}

void WindowImpl::SetPixelBounds(const Rect& bounds) {
  SetWindowPos(hwnd(), NULL,
               bounds.x(), bounds.y(), bounds.width(), bounds.height(),
               SWP_NOACTIVATE | SWP_NOZORDER);
}

Rect WindowImpl::GetPixelBounds() {
  RECT r;
  GetWindowRect(hwnd(), &r);
  return Rect(r);
}

Rect WindowImpl::GetContentPixelBounds() {
  RECT r;
  GetClientRect(hwnd(), &r);
  POINT point = { r.left, r.top };
  ClientToScreen(hwnd(), &point);
  return Rect(point.x, point.y, r.right - r.left, r.bottom - r.top);
}

void WindowImpl::AdvanceFocus() {
  if (delegate_->GetContentView()->GetClassName() == Container::kClassName)
    focus_manager_.AdvanceFocus(
        static_cast<Container*>(delegate_->GetContentView()), IsShiftPressed());
}

void WindowImpl::SetCapture(ViewImpl* view) {
  captured_view_ = view;
  ::SetCapture(hwnd());
}

void WindowImpl::ReleaseCapture() {
  if (::GetCapture() == hwnd())
    ::ReleaseCapture();
}

void WindowImpl::SetBackgroundColor(nu::Color color) {
  background_color_ = color;
  ::InvalidateRect(hwnd(), NULL, TRUE);
}

bool WindowImpl::IsMaximized() const {
  return !!::IsZoomed(hwnd()) && !IsFullscreen();
}

bool WindowImpl::IsFullscreen() const {
  return false;
}

void WindowImpl::SetWindowStyle(LONG style, bool on) {
  LONG styles = ::GetWindowLong(hwnd(), GWL_STYLE);
  if (on)
    styles |= style;
  else
    styles &= ~style;
  ::SetWindowLong(hwnd(), GWL_STYLE, styles);
}

bool WindowImpl::HasWindowStyle(LONG style) const {
  return (::GetWindowLong(hwnd(), GWL_STYLE) & style) != 0;
}

void WindowImpl::OnCaptureChanged(HWND window) {
  if (captured_view_) {
    captured_view_->OnCaptureLost();
    captured_view_ = nullptr;
  }
}

void WindowImpl::OnClose() {
  if (delegate_->should_close.is_null() || delegate_->should_close.Run()) {
    delegate_->on_close.Emit();
    SetMsgHandled(false);
  }
}

void WindowImpl::OnCommand(UINT code, int command, HWND window) {
  if (!code && !window && delegate_->GetMenu()) {
    DispatchCommandToItem(delegate_->GetMenu(), command);
    return;
  } else if (::GetParent(window) != hwnd()) {
    LOG(ERROR) << "Received notification " << code << " " << command
               << " from a non-child window";
    return;
  }

  auto* control = reinterpret_cast<SubwinView*>(GetWindowUserData(window));
  control->OnCommand(code, command);
}

void WindowImpl::OnSize(UINT param, const Size& size) {
  if (!delegate_->GetContentView())
    return;
  delegate_->GetContentView()->GetNative()->SizeAllocate(Rect(size));
  RedrawWindow(hwnd(), NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

LRESULT WindowImpl::OnDPIChanged(UINT msg, WPARAM w_param, LPARAM l_param) {
  float new_scale_factor = GetScalingFactorFromDPI(LOWORD(w_param));
  if (new_scale_factor != scale_factor_) {
    scale_factor_ = new_scale_factor;
    // Notify the content view of DPI change.
    delegate_->GetContentView()->GetNative()->BecomeContentView(this);
    // Move to the new window position under new DPI.
    SetPixelBounds(Rect(*reinterpret_cast<RECT*>(l_param)));
  }
  return 1;
}

LRESULT WindowImpl::OnMouseMove(UINT message, WPARAM w_param, LPARAM l_param) {
  Win32Message msg = {message, w_param, l_param};
  if (!mouse_in_window_) {
    mouse_in_window_ = true;
    if (!captured_view_)
      delegate_->GetContentView()->GetNative()->OnMouseEnter(&msg);
    TrackMouse(true);
  }

  if (captured_view_) {
    captured_view_->OnMouseMove(&msg);
    return 0;
  }

  delegate_->GetContentView()->GetNative()->OnMouseMove(&msg);
  return 0;
}

LRESULT WindowImpl::OnMouseLeave(UINT message, WPARAM w_param, LPARAM l_param) {
  TrackMouse(false);
  mouse_in_window_ = false;

  Win32Message msg = {message, w_param, l_param};
  if (captured_view_) {
    captured_view_->OnMouseLeave(&msg);
    return 0;
  }

  delegate_->GetContentView()->GetNative()->OnMouseLeave(&msg);
  return 0;
}

BOOL WindowImpl::OnMouseWheel(bool vertical, UINT flags, int delta,
                                  const Point& point) {
  POINT p = point.ToPOINT();
  ScreenToClient(hwnd(), &p);
  return delegate_->GetContentView()->GetNative()->OnMouseWheel(
      vertical, flags, delta, Point(p));
}

LRESULT WindowImpl::OnMouseClick(UINT message, WPARAM w_param, LPARAM l_param) {
  Win32Message msg = {message, w_param, l_param};
  if (captured_view_) {
    captured_view_->OnMouseClick(&msg);
    return 0;
  }

  // Pass the event to view.
  if (!delegate_->GetContentView()->GetNative()->OnMouseClick(&msg))
    SetMsgHandled(false);
  return 0;
}

LRESULT WindowImpl::OnKeyEvent(UINT message, WPARAM w_param, LPARAM l_param) {
  // First pass the event to view.
  Win32Message msg = {message, w_param, l_param};
  if (focus_manager()->focused_view() &&
      focus_manager()->focused_view()->GetNative()->OnKeyEvent(&msg))
    return 0;

  // If no one handles it then pass the event to menu.
  KeyEvent event(&msg, delegate_->GetContentView()->GetNative());
  if (event.type == EventType::KeyDown && delegate_->GetMenu()) {
    Accelerator accelerator(event);
    int command = delegate_->GetMenu()->accel_manager()->Process(accelerator);
    if (command != -1) {
      DispatchCommandToItem(delegate_->GetMenu(), command);
      return 0;
    }
  }
  SetMsgHandled(false);
  return 0;
}

void WindowImpl::OnChar(UINT ch, UINT repeat, UINT flags) {
  if (ch == VK_TAB)
    AdvanceFocus();
}

void WindowImpl::OnPaint(HDC) {
  PAINTSTRUCT ps;
  BeginPaint(hwnd(), &ps);

  // Always update the whole buffer for transparent window.
  Rect bounds(GetContentPixelBounds());
  Rect dirty;
  if (delegate_->IsTransparent())
    dirty = Rect(bounds.size());
  else
    dirty = Rect(ps.rcPaint);

  base::win::ScopedGetDC dc(hwnd());
  {
    // Double buffering the drawing.
    DoubleBuffer buffer(dc, bounds.size(), dirty, dirty.origin());

    // Draw.
    {
      // Background.
      PainterWin painter(buffer.dc(), scale_factor_);
      painter.SetColor(background_color_);
      painter.FillRectPixel(dirty);

      // Controls.
      delegate_->GetContentView()->GetNative()->Draw(&painter, dirty);
    }

    // Update layered window.
    if (delegate_->IsTransparent()) {
      RECT wr;
      ::GetWindowRect(hwnd(), &wr);
      SIZE size = {wr.right - wr.left, wr.bottom - wr.top};
      POINT position = {wr.left, wr.top};
      POINT zero = {0, 0};
      BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};

      buffer.SetNoCopy();
      ::UpdateLayeredWindow(hwnd(), NULL, &position, &size, buffer.dc(),
                            &zero, 0, &blend, ULW_ALPHA);
    }
  }

  EndPaint(hwnd(), &ps);
}

LRESULT WindowImpl::OnEraseBkgnd(HDC dc) {
  // Needed to prevent resize flicker.
  return 1;
}

HBRUSH WindowImpl::OnCtlColorStatic(HDC dc, HWND window) {
  auto* control = reinterpret_cast<SubwinView*>(GetWindowUserData(window));
  HBRUSH brush = NULL;
  SetMsgHandled(control->OnCtlColor(dc, &brush));
  return brush;
}

LRESULT WindowImpl::OnNCHitTest(UINT msg, WPARAM w_param, LPARAM l_param) {
  // Only override this for frameless window.
  if (delegate_->HasFrame()) {
    SetMsgHandled(false);
    return 0;
  }

  // Do nothing for non-client area for now.
  LRESULT hit = ::DefWindowProc(hwnd(), msg, w_param, l_param);
  if (hit != HTCLIENT)
    return hit;

  // Convert point to client area.
  POINT temp = { CR_GET_X_LPARAM(l_param), CR_GET_Y_LPARAM(l_param) };
  ::MapWindowPoints(HWND_DESKTOP, hwnd(), &temp, 1);
  Point point(temp);

  // Calculate the resize handle.
  if (delegate_->IsResizable() && !(IsMaximized() || IsFullscreen())) {
    Rect bounds = GetPixelBounds();
    int border_thickness = kResizeBorderWidth * scale_factor();
    int corner_width = kResizeCornerWidth * scale_factor();
    if (point.x() < border_thickness) {
      if (point.y() < corner_width)
        return HTTOPLEFT;
      else if (point.y() >= (bounds.height() - border_thickness))
        return HTBOTTOMLEFT;
      else
        return HTLEFT;
    } else if (point.x() >= (bounds.width() - border_thickness)) {
      if (point.y() < corner_width)
        return HTTOPRIGHT;
      else if (point.y() >= (bounds.height() - border_thickness))
        return HTBOTTOMRIGHT;
      else
        return HTRIGHT;
    } else if (point.y() < border_thickness) {
      if (point.x() < corner_width)
        return HTTOPLEFT;
      else if (point.x() >= (bounds.width() - corner_width))
        return HTTOPRIGHT;
      else
        return HTTOP;
    } else if (point.y() >= (bounds.height() - border_thickness)) {
      if (point.x() < corner_width)
        return HTBOTTOMLEFT;
      else if (point.x() >= (bounds.width() - corner_width))
        return HTBOTTOMRIGHT;
      else
        return HTBOTTOM;
    }
  }

  // Get result from content view.
  return delegate_->GetContentView()->GetNative()->HitTest(point);
}

LRESULT WindowImpl::OnNCCalcSize(BOOL mode, LPARAM l_param) {
  // Let User32 handle the first nccalcsize for captioned windows
  // so it updates its internal structures (specifically caption-present)
  // Without this Tile & Cascade windows won't work.
  // See http://code.google.com/p/chromium/issues/detail?id=900
  if (is_first_nccalc_) {
    is_first_nccalc_ = false;
    if (HasWindowStyle(WS_CAPTION)) {
      SetMsgHandled(false);
      return 0;
    }
  }

  Insets insets;
  bool got_insets = GetClientAreaInsets(&insets);
  if (!got_insets && !IsFullscreen() && !(mode && !delegate_->HasFrame())) {
    SetMsgHandled(FALSE);
    return 0;
  }

  RECT* client_rect = mode ?
      &(reinterpret_cast<NCCALCSIZE_PARAMS*>(l_param)->rgrc[0]) :
      reinterpret_cast<RECT*>(l_param);
  client_rect->left += insets.left();
  client_rect->top += insets.top();
  client_rect->bottom -= insets.bottom();
  client_rect->right -= insets.right();

  // If the window bounds change, we're going to relayout and repaint anyway.
  // Returning WVR_REDRAW avoids an extra paint before that of the old client
  // pixels in the (now wrong) location, and thus makes actions like resizing a
  // window from the left edge look slightly less broken.
  // We special case when left or top insets are 0, since these conditions
  // actually require another repaint to correct the layout after glass gets
  // turned on and off.
  if (insets.left() == 0 || insets.top() == 0)
    return 0;
  return mode ? WVR_REDRAW : 0;
}

LRESULT WindowImpl::OnSetCursor(UINT message, WPARAM w_param, LPARAM l_param) {
  // Reimplement the necessary default behavior here. Calling DefWindowProc can
  // trigger weird non-client painting for non-glass windows with custom frames.
  // Using a ScopedRedrawLock to prevent caption rendering artifacts may allow
  // content behind this window to incorrectly paint in front of this window.
  // Invalidating the window to paint over either set of artifacts is not ideal.
  wchar_t* cursor = IDC_ARROW;
  switch (LOWORD(l_param)) {
    case HTSIZE:
      cursor = IDC_SIZENWSE;
      break;
    case HTLEFT:
    case HTRIGHT:
      cursor = IDC_SIZEWE;
      break;
    case HTTOP:
    case HTBOTTOM:
      cursor = IDC_SIZENS;
      break;
    case HTTOPLEFT:
    case HTBOTTOMRIGHT:
      cursor = IDC_SIZENWSE;
      break;
    case HTTOPRIGHT:
    case HTBOTTOMLEFT:
      cursor = IDC_SIZENESW;
      break;
    case LOWORD(HTERROR):  // Use HTERROR's LOWORD value for valid comparison.
      SetMsgHandled(false);
      break;
    default:
      // Use the default value, IDC_ARROW.
      break;
  }
  ::SetCursor(::LoadCursor(NULL, cursor));
  return 1;
}

void WindowImpl::TrackMouse(bool enable) {
  TRACKMOUSEEVENT event = {0};
  event.cbSize = sizeof(event);
  event.hwndTrack = hwnd();
  event.dwFlags = (enable ? 0 : TME_CANCEL) | TME_LEAVE;
  event.dwHoverTime = 0;
  TrackMouseEvent(&event);
}

bool WindowImpl::GetClientAreaInsets(Insets* insets) {
  // Returning false causes the default handling in OnNCCalcSize() to
  // be invoked.
  if (HasSystemFrame())
    return false;

  if (IsMaximized()) {
    // Windows automatically adds a standard width border to all sides when a
    // window is maximized.
    int border_thickness = ::GetSystemMetrics(SM_CXSIZEFRAME);
    if (!delegate_->HasFrame())
      border_thickness -= 1;
    *insets = Insets(border_thickness, border_thickness,
                     border_thickness, border_thickness);
    return true;
  }

  *insets = Insets();
  return true;
}

bool WindowImpl::HasSystemFrame() const {
  // In future we may support custom non-client frame.
  return delegate_->HasFrame();
}

///////////////////////////////////////////////////////////////////////////////
// Public Window API implementation.

void Window::PlatformInit(const Options& options) {
  window_ = new WindowImpl(options, this);
  if (!options.bounds.IsEmpty())
    SetBounds(options.bounds);

  YGConfigSetPointScaleFactor(yoga_config_,
                              GetScaleFactorForHWND(window_->hwnd()));
}

void Window::PlatformDestroy() {
  delete window_;
}

void Window::Close() {
  ::SendMessage(window_->hwnd(), WM_CLOSE, 0, 0);
}

void Window::PlatformSetContentView(View* view) {
  view->GetNative()->BecomeContentView(window_);
  view->SetPixelBounds(Rect(window_->GetContentPixelBounds().size()));
}

void Window::PlatformSetMenu(MenuBar* menu_bar) {
  ::SetMenu(window_->hwnd(), menu_bar ? menu_bar->GetNative() : NULL);
}

void Window::SetContentBounds(const RectF& bounds) {
  Rect prect = ToNearestRect(ScaleRect(bounds, window_->scale_factor()));
  if (HasFrame())
    window_->SetPixelBounds(ContentToWindowBounds(window_, !!menu_bar_, prect));
  else
    window_->SetPixelBounds(prect);
}

RectF Window::GetContentBounds() const {
  return ScaleRect(RectF(window_->GetContentPixelBounds()),
                   1.0f / window_->scale_factor());
}

void Window::SetBounds(const RectF& bounds) {
  window_->SetPixelBounds(
      ToNearestRect(ScaleRect(bounds, window_->scale_factor())));
}

RectF Window::GetBounds() const {
  return ScaleRect(RectF(window_->GetPixelBounds()),
                         1.0f / window_->scale_factor());
}

void Window::SetVisible(bool visible) {
  ShowWindow(window_->hwnd(), visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

bool Window::IsVisible() const {
  return !!::IsWindowVisible(window_->hwnd());
}

void Window::SetResizable(bool yes) {
  window_->SetWindowStyle(WS_THICKFRAME, yes);
}

bool Window::IsResizable() const {
  return window_->HasWindowStyle(WS_THICKFRAME);
}

void Window::SetMaximizable(bool yes) {
  window_->SetWindowStyle(WS_MAXIMIZEBOX, yes);
}

bool Window::IsMaximizable() const {
  return window_->HasWindowStyle(WS_MAXIMIZEBOX);
}

void Window::SetBackgroundColor(Color color) {
  window_->SetBackgroundColor(color);
}

}  // namespace nu
