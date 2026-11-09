/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

#ifndef WAYLANDSTART_H
#define WAYLANDSTART_H

#include <QString>
#include <QMap>

// The Wayland session starts in two stages:
//
//   1. `lingmo-session --wayland` (SDDM, wayland-sessions/lingmo-wayland.desktop):
//      no display yet. Prepares the environment and kwinrc, writes the start marker
//      and execs `kwin_wayland_wrapper --xwayland --exit-with-session
//      "lingmo-session --wayland-child"`.
//   2. `lingmo-session --wayland-child` (started by KWin): daemons, shell, autostart.
//      Clears the marker once the status bar is on the bus.
//
// Start marker: $XDG_STATE_HOME/lingmo-session/wayland-start (key=value lines).
// When it is still there at the next login the Wayland start failed: the root
// helper run before the SDDM greeter (lingmo-wayland-check greeter) points the
// greeter back at the X11 session, and the X11 session tells the user why
// (the marker becomes wayland-failed, which Settings and Lingmo.System read).
namespace WaylandStart
{
// Stage 1. Only returns on failure.
int exec(int argc, char *argv[]);

// Why the start failed, as written in the marker
//   nvidia-modeset  NVIDIA driver without nvidia_drm.modeset=1
//   nvidia-legacy   NVIDIA driver older than 495 (no GBM)
//   compositor      KWin never started stage 2
//   shell           stage 2 ran but the status bar never came up
QString markerDir();
QString markerPath();
QString failedPath();
QMap<QString, QString> readMarker(const QString &path);
void writeMarker(const QMap<QString, QString> &values);
void markChildStarted();
void clearMarker();

// Empty when this GPU/driver can run the Wayland session, else a reason above
QString unsupportedReason();

// No GPU device (VMs without 3D): QtQuick and KWin render in software
void applySoftwareRenderingFallback(bool wayland);
}

#endif // WAYLANDSTART_H
