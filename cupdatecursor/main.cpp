// cupdatecursor <theme> <size>: the cursor theme for KWin (kcminputrc, both
// sessions) and, in the X11 session, for the X server's root window.
#include <QGuiApplication>
#include <QtGui/private/qtx11extras_p.h>
#include <QFile>
#include <QDebug>

#include <KConfig>
#include <KConfigGroup>

#include <X11/X.h>
#include <X11/Xcursor/Xcursor.h>

// KWin reads the cursor from kcminputrc (on Wayland it draws every cursor itself).
// KConfig, not QSettings: KWin keeps per-device input groups in this file
// ([Libinput][vendor][product][name]) that QSettings would mangle.
static void writeKcminputrc(const QString &theme, int size)
{
    KConfig config(QStringLiteral("kcminputrc"));
    KConfigGroup mouse = config.group(QStringLiteral("Mouse"));
    if (!theme.isEmpty())
        mouse.writeEntry("cursorTheme", theme, KConfig::Notify);
    if (size > 0)
        mouse.writeEntry("cursorSize", size, KConfig::Notify);
    config.sync();
}

static void applyX11Theme(const QString &theme, int size)
{
    Display *display = QX11Info::display();
    if (!display)
        return;

    if (!theme.isEmpty())
        XcursorSetTheme(display, QFile::encodeName(theme));

    if (size > 0)
        XcursorSetDefaultSize(display, size);

    Cursor handle = XcursorLibraryLoadCursor(display, "left_ptr");
    XDefineCursor(display, DefaultRootWindow(display), handle);
    XFreeCursor(display, handle);
    XFlush(display);
}

int main(int argc, char *argv[])
{
    if (argc != 3)
        return 1;

    const QString theme = QFile::decodeName(argv[1]);
    const int size = QFile::decodeName(argv[2]).toInt();

    // Always, and before any display is needed: the Wayland session's first stage
    // runs this before KWin (and so any display) exists
    writeKcminputrc(theme, size);

    // X11 session only: the root window cursor (Xwayland's belongs to KWin)
    if (qgetenv("XDG_SESSION_TYPE") == "wayland" || !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")
        || qEnvironmentVariableIsEmpty("DISPLAY"))
        return 0;

    qputenv("QT_QPA_PLATFORM", "xcb");
    QGuiApplication::setDesktopSettingsAware(false);
    QGuiApplication a(argc, argv);

    if (!QX11Info::isPlatformX11())
        return 2;

    applyX11Theme(theme, size);

    return 0;
}
