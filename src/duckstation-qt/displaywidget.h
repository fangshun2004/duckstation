// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "util/window_info.h"

#include "common/types.h"

#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QWidget>
#include <optional>

class QCloseEvent;

class DisplayWidget final : public QWidget
{
  Q_OBJECT

public:
  explicit DisplayWidget(QWidget* parent);
  ~DisplayWidget();

  QPaintEngine* paintEngine() const override;

  int scaledWindowWidth() const;
  int scaledWindowHeight() const;

  std::optional<WindowInfo> getWindowInfo();

  void updateRelativeMode(bool enabled);
  void updateCursor(bool hidden);

  void handleCloseEvent(QCloseEvent* event);
  void destroy();

Q_SIGNALS:
  void windowResizedEvent(int width, int height, float scale);
  void windowRestoredEvent();
  void windowKeyEvent(int key_code, bool pressed);
  void windowTextEntered(const QString& text);
  void windowMouseButtonEvent(int button, bool pressed);
  void windowMouseWheelEvent(const QPoint& angle_delta);

protected:
  bool event(QEvent* event) override;

private:
  void updateCenterPos();

  QPoint m_relative_mouse_start_pos{};
  QPoint m_relative_mouse_center_pos{};
  bool m_relative_mouse_enabled = false;
#ifdef _WIN32
  bool m_clip_mouse_enabled = false;
#endif
  bool m_cursor_hidden = false;
  bool m_destroying = false;

  std::vector<u32> m_keys_pressed_with_modifiers;

  u32 m_last_window_width = 0;
  u32 m_last_window_height = 0;
  float m_last_window_scale = 1.0f;
};

class DisplayContainer final : public QStackedWidget
{
  Q_OBJECT

public:
  DisplayContainer();
  ~DisplayContainer();

  // Wayland is broken in lots of ways, so we need to check for it.
  static bool isRunningOnWayland();

  static bool isNeeded(bool fullscreen, bool render_to_main);

  void setDisplayWidget(DisplayWidget* widget);
  DisplayWidget* removeDisplayWidget();

protected:
  bool event(QEvent* event) override;

private:
  DisplayWidget* m_display_widget = nullptr;
};
