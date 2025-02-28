// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "displaywidget.h"
#include "common/assert.h"
#include "common/bitutils.h"
#include "common/log.h"
#include "mainwindow.h"
#include "qthost.h"
#include "qtutils.h"
#include "util/imgui_manager.h"
#include <QtCore/QDebug>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QScreen>
#include <QtGui/QWindow>
#include <QtGui/QWindowStateChangeEvent>
#include <cmath>

#if !defined(_WIN32) && !defined(APPLE)
#include <qpa/qplatformnativeinterface.h>
#endif

#ifdef _WIN32
#include "common/windows_headers.h"
#endif

Log_SetChannel(DisplayWidget);

DisplayWidget::DisplayWidget(QWidget* parent) : QWidget(parent)
{
  // We want a native window for both D3D and OpenGL.
  setAutoFillBackground(false);
  setAttribute(Qt::WA_NativeWindow, true);
  setAttribute(Qt::WA_NoSystemBackground, true);
  setAttribute(Qt::WA_PaintOnScreen, true);
  setAttribute(Qt::WA_KeyCompression, false);
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
}

DisplayWidget::~DisplayWidget()
{
#ifdef _WIN32
  if (m_clip_mouse_enabled)
    ClipCursor(nullptr);
#endif
}

int DisplayWidget::scaledWindowWidth() const
{
  return std::max(
    static_cast<int>(std::ceil(static_cast<qreal>(width()) * QtUtils::GetDevicePixelRatioForWidget(this))), 1);
}

int DisplayWidget::scaledWindowHeight() const
{
  return std::max(
    static_cast<int>(std::ceil(static_cast<qreal>(height()) * QtUtils::GetDevicePixelRatioForWidget(this))), 1);
}

std::optional<WindowInfo> DisplayWidget::getWindowInfo()
{
  std::optional<WindowInfo> ret(QtUtils::GetWindowInfoForWidget(this));
  if (ret.has_value())
  {
    m_last_window_width = ret->surface_width;
    m_last_window_height = ret->surface_height;
    m_last_window_scale = ret->surface_scale;
  }
  return ret;
}

void DisplayWidget::updateRelativeMode(bool enabled)
{
#ifdef _WIN32
  // prefer ClipCursor() over warping movement when we're using raw input
  bool clip_cursor = enabled && InputManager::IsUsingRawInput();
  if (m_relative_mouse_enabled == enabled && m_clip_mouse_enabled == clip_cursor)
    return;

  Log_InfoPrintf("updateRelativeMode(): relative=%s, clip=%s", enabled ? "yes" : "no", clip_cursor ? "yes" : "no");

  if (!clip_cursor && m_clip_mouse_enabled)
  {
    m_clip_mouse_enabled = false;
    ClipCursor(nullptr);
  }
#else
  if (m_relative_mouse_enabled == enabled)
    return;

  Log_InfoPrintf("updateRelativeMode(): relative=%s", enabled ? "yes" : "no");
#endif

  if (enabled)
  {
#ifdef _WIN32
    m_relative_mouse_enabled = !clip_cursor;
    m_clip_mouse_enabled = clip_cursor;
#else
    m_relative_mouse_enabled = true;
#endif
    m_relative_mouse_start_pos = QCursor::pos();
    updateCenterPos();
    grabMouse();
  }
  else if (m_relative_mouse_enabled)
  {
    m_relative_mouse_enabled = false;
    QCursor::setPos(m_relative_mouse_start_pos);
    releaseMouse();
  }
}

void DisplayWidget::updateCursor(bool hidden)
{
  if (m_cursor_hidden == hidden)
    return;

  m_cursor_hidden = hidden;
  if (hidden)
  {
    Log_DevPrintf("updateCursor(): Cursor is now hidden");
    setCursor(Qt::BlankCursor);
  }
  else
  {
    Log_DevPrintf("updateCursor(): Cursor is now shown");
    unsetCursor();
  }
}

void DisplayWidget::handleCloseEvent(QCloseEvent* event)
{
  // Closing the separate widget will either cancel the close, or trigger shutdown.
  // In the latter case, it's going to destroy us, so don't let Qt do it first.
  if (QtHost::IsSystemValid())
  {
    QMetaObject::invokeMethod(g_main_window, "requestShutdown", Q_ARG(bool, true), Q_ARG(bool, true),
                              Q_ARG(bool, false));
  }
  else
  {
    QMetaObject::invokeMethod(g_main_window, "requestExit");
  }

  event->ignore();
}

void DisplayWidget::destroy()
{
  m_destroying = true;

#ifdef __APPLE__
  // See Qt documentation, entire application is in full screen state, and the main
  // window will get reopened fullscreen instead of windowed if we don't close the
  // fullscreen window first.
  if (isFullScreen())
    close();
#endif
  deleteLater();
}

void DisplayWidget::updateCenterPos()
{
#ifdef _WIN32
  if (m_clip_mouse_enabled)
  {
    RECT rc;
    if (GetWindowRect(reinterpret_cast<HWND>(winId()), &rc))
      ClipCursor(&rc);
  }
  else if (m_relative_mouse_enabled)
  {
    RECT rc;
    if (GetWindowRect(reinterpret_cast<HWND>(winId()), &rc))
    {
      m_relative_mouse_center_pos.setX(((rc.right - rc.left) / 2) + rc.left);
      m_relative_mouse_center_pos.setY(((rc.bottom - rc.top) / 2) + rc.top);
      SetCursorPos(m_relative_mouse_center_pos.x(), m_relative_mouse_center_pos.y());
    }
  }
#else
  if (m_relative_mouse_enabled)
  {
    // we do a round trip here because these coordinates are dpi-unscaled
    m_relative_mouse_center_pos = mapToGlobal(QPoint((width() + 1) / 2, (height() + 1) / 2));
    QCursor::setPos(m_relative_mouse_center_pos);
    m_relative_mouse_center_pos = QCursor::pos();
  }
#endif
}

QPaintEngine* DisplayWidget::paintEngine() const
{
  return nullptr;
}

bool DisplayWidget::event(QEvent* event)
{
  switch (event->type())
  {
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    {
      const QKeyEvent* key_event = static_cast<QKeyEvent*>(event);

      if (ImGuiManager::WantsTextInput() && key_event->type() == QEvent::KeyPress)
      {
        // Don't forward backspace characters. We send the backspace as a normal key event,
        // so if we send the character too, it double-deletes.
        QString text(key_event->text());
        text.remove(QChar('\b'));
        if (!text.isEmpty())
          emit windowTextEntered(text);
      }

      if (key_event->isAutoRepeat())
        return true;

      // For some reason, Windows sends "fake" key events.
      // Scenario: Press shift, press F1, release shift, release F1.
      // Events: Shift=Pressed, F1=Pressed, Shift=Released, **F1=Pressed**, F1=Released.
      // To work around this, we keep track of keys pressed with modifiers in a list, and
      // discard the press event when it's been previously activated. It's pretty gross,
      // but I can't think of a better way of handling it, and there doesn't appear to be
      // any window flag which changes this behavior that I can see.

      const u32 key = QtUtils::KeyEventToCode(key_event);
      const Qt::KeyboardModifiers modifiers = key_event->modifiers();
      const bool pressed = (key_event->type() == QEvent::KeyPress);
      const auto it = std::find(m_keys_pressed_with_modifiers.begin(), m_keys_pressed_with_modifiers.end(), key);
      if (it != m_keys_pressed_with_modifiers.end())
      {
        if (pressed)
          return true;
        else
          m_keys_pressed_with_modifiers.erase(it);
      }
      else if (modifiers != Qt::NoModifier && modifiers != Qt::KeypadModifier && pressed)
      {
        m_keys_pressed_with_modifiers.push_back(key);
      }

      emit windowKeyEvent(key, pressed);
      return true;
    }

    case QEvent::MouseMove:
    {
      const QMouseEvent* mouse_event = static_cast<QMouseEvent*>(event);

      if (!m_relative_mouse_enabled)
      {
        const qreal dpr = QtUtils::GetDevicePixelRatioForWidget(this);
        const QPoint mouse_pos = mouse_event->pos();

        const float scaled_x = static_cast<float>(static_cast<qreal>(mouse_pos.x()) * dpr);
        const float scaled_y = static_cast<float>(static_cast<qreal>(mouse_pos.y()) * dpr);
        InputManager::UpdatePointerAbsolutePosition(0, scaled_x, scaled_y);
      }
      else
      {
        // On windows, we use winapi here. The reason being that the coordinates in QCursor
        // are un-dpi-scaled, so we lose precision at higher desktop scalings.
        float dx = 0.0f, dy = 0.0f;

#ifndef _WIN32
        const QPoint mouse_pos = QCursor::pos();
        if (mouse_pos != m_relative_mouse_center_pos)
        {
          dx = static_cast<float>(mouse_pos.x() - m_relative_mouse_center_pos.x());
          dy = static_cast<float>(mouse_pos.y() - m_relative_mouse_center_pos.y());
          QCursor::setPos(m_relative_mouse_center_pos);
        }
#else
        POINT mouse_pos;
        if (GetCursorPos(&mouse_pos))
        {
          dx = static_cast<float>(mouse_pos.x - m_relative_mouse_center_pos.x());
          dy = static_cast<float>(mouse_pos.y - m_relative_mouse_center_pos.y());
          SetCursorPos(m_relative_mouse_center_pos.x(), m_relative_mouse_center_pos.y());
        }
#endif

        if (dx != 0.0f)
          InputManager::UpdatePointerRelativeDelta(0, InputPointerAxis::X, dx);
        if (dy != 0.0f)
          InputManager::UpdatePointerRelativeDelta(0, InputPointerAxis::Y, dy);
      }

      return true;
    }

    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseButtonRelease:
    {
      const u32 button_index = CountTrailingZeros(static_cast<u32>(static_cast<const QMouseEvent*>(event)->button()));
      emit windowMouseButtonEvent(static_cast<int>(button_index), event->type() != QEvent::MouseButtonRelease);

      // don't toggle fullscreen when we're bound.. that wouldn't end well.
      if (event->type() == QEvent::MouseButtonDblClick &&
          static_cast<const QMouseEvent*>(event)->button() == Qt::LeftButton &&
          !InputManager::HasAnyBindingsForKey(InputManager::MakePointerButtonKey(0, 0)) && QtHost::IsSystemValid() &&
          !QtHost::IsSystemPaused() && Host::GetBoolSettingValue("Main", "DoubleClickTogglesFullscreen", true))
      {
        g_emu_thread->toggleFullscreen();
      }

      return true;
    }

    case QEvent::Wheel:
    {
      const QWheelEvent* wheel_event = static_cast<QWheelEvent*>(event);
      emit windowMouseWheelEvent(wheel_event->angleDelta());
      return true;
    }

      // According to https://bugreports.qt.io/browse/QTBUG-95925 the recommended practice for handling DPI change is
      // responding to paint events
    case QEvent::Paint:
    case QEvent::Resize:
    {
      QWidget::event(event);

      const float dpr = QtUtils::GetDevicePixelRatioForWidget(this);
      const u32 scaled_width =
        static_cast<u32>(std::max(static_cast<int>(std::ceil(static_cast<qreal>(width()) * dpr)), 1));
      const u32 scaled_height =
        static_cast<u32>(std::max(static_cast<int>(std::ceil(static_cast<qreal>(height()) * dpr)), 1));

      // avoid spamming resize events for paint events (sent on move on windows)
      if (m_last_window_width != scaled_width || m_last_window_height != scaled_height || m_last_window_scale != dpr)
      {
        m_last_window_width = scaled_width;
        m_last_window_height = scaled_height;
        m_last_window_scale = dpr;
        emit windowResizedEvent(scaled_width, scaled_height, dpr);
      }

      updateCenterPos();
      return true;
    }

    case QEvent::Move:
    {
      updateCenterPos();
      return true;
    }

    case QEvent::Close:
    {
      if (m_destroying)
        return QWidget::event(event);

      handleCloseEvent(static_cast<QCloseEvent*>(event));
      return true;
    }

    case QEvent::WindowStateChange:
    {
      QWidget::event(event);

      if (static_cast<QWindowStateChangeEvent*>(event)->oldState() & Qt::WindowMinimized)
        emit windowRestoredEvent();

      return true;
    }

    default:
      return QWidget::event(event);
  }
}

DisplayContainer::DisplayContainer() : QStackedWidget(nullptr)
{
}

DisplayContainer::~DisplayContainer() = default;

bool DisplayContainer::isNeeded(bool fullscreen, bool render_to_main)
{
#if defined(_WIN32) || defined(__APPLE__)
  return false;
#else
  if (!isRunningOnWayland())
    return false;

  // We only need this on Wayland because of client-side decorations...
  return (fullscreen || !render_to_main);
#endif
}

bool DisplayContainer::isRunningOnWayland()
{
#if defined(_WIN32) || defined(__APPLE__)
  return false;
#else
  const QString platform_name = QGuiApplication::platformName();
  return (platform_name == QStringLiteral("wayland"));
#endif
}

void DisplayContainer::setDisplayWidget(DisplayWidget* widget)
{
  Assert(!m_display_widget);
  m_display_widget = widget;
  addWidget(widget);
}

DisplayWidget* DisplayContainer::removeDisplayWidget()
{
  DisplayWidget* widget = m_display_widget;
  Assert(widget);
  m_display_widget = nullptr;
  removeWidget(widget);
  return widget;
}

bool DisplayContainer::event(QEvent* event)
{
  if (event->type() == QEvent::Close && m_display_widget)
  {
    m_display_widget->handleCloseEvent(static_cast<QCloseEvent*>(event));
    return true;
  }

  const bool res = QStackedWidget::event(event);
  if (!m_display_widget)
    return res;

  switch (event->type())
  {
    case QEvent::WindowStateChange:
    {
      if (static_cast<QWindowStateChangeEvent*>(event)->oldState() & Qt::WindowMinimized)
        emit m_display_widget->windowRestoredEvent();
    }
    break;

    default:
      break;
  }

  return res;
}
