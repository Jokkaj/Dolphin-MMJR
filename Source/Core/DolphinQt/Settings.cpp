// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "DolphinQt/Settings.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QSize>
#include <QWidget>

#include "AudioCommon/AudioCommon.h"

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"

#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/IOS/IOS.h"
#include "Core/NetPlayClient.h"
#include "Core/NetPlayServer.h"

#include "DolphinQt/Host.h"
#include "DolphinQt/QtUtils/QueueOnObject.h"

#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/InputConfig.h"

#include "VideoCommon/NetPlayChatUI.h"
#include "VideoCommon/NetPlayGolfUI.h"
#include "VideoCommon/RenderBase.h"

Settings::Settings()
{
  qRegisterMetaType<Core::State>();
  Core::AddOnStateChangedCallback([this](Core::State new_state) {
    QueueOnObject(this, [this, new_state] { emit EmulationStateChanged(new_state); });
  });

  Config::AddConfigChangedCallback(
      [this] { QueueOnObject(this, [this] { emit ConfigChanged(); }); });

  g_controller_interface.RegisterDevicesChangedCallback([this] {
    if (Host::GetInstance()->IsHostThread())
    {
      emit DevicesChanged();
    }
    else
    {
      // Any device shared_ptr in the host thread needs to be released immediately as otherwise
      // they'd continue living until the queued event has run, but some devices can't be recreated
      // until they are destroyed.
      // This is safe from any thread. Devices will be refreshed and re-acquired and in
      // DevicesChanged(). Waiting on QueueOnObject() to have finished running was not feasible as
      // it would cause deadlocks without heavy workarounds.
      emit ReleaseDevices();

      QueueOnObject(this, [this] { emit DevicesChanged(); });
    }
  });
}

Settings::~Settings() = default;

Settings& Settings::Instance()
{
  static Settings settings;
  return settings;
}

QSettings& Settings::GetQSettings()
{
  static QSettings settings(
      QStringLiteral("%1/Qt.ini").arg(QString::fromStdString(File::GetUserPath(D_CONFIG_IDX))),
      QSettings::IniFormat);
  return settings;
}

void Settings::SetThemeName(const QString& theme_name)
{
  SConfig::GetInstance().theme_name = theme_name.toStdString();
  emit ThemeChanged();
}

QString Settings::GetCurrentUserStyle() const
{
  if (GetQSettings().contains(QStringLiteral("userstyle/name")))
    return GetQSettings().value(QStringLiteral("userstyle/name")).toString();

  // Migration code for the old way of storing this setting
  return QFileInfo(GetQSettings().value(QStringLiteral("userstyle/path")).toString()).fileName();
}

// Calling this before the main window has been created breaks the style of some widgets on
// Windows 10/Qt 5.15.0. But only if we set a stylesheet that isn't an empty string.
void Settings::SetCurrentUserStyle(const QString& stylesheet_name)
{
  QString stylesheet_contents;

  // If we haven't found one, we continue with an empty (default) style
  if (!stylesheet_name.isEmpty() && AreUserStylesEnabled())
  {
    // Load custom user stylesheet
    QDir directory = QDir(QString::fromStdString(File::GetUserPath(D_STYLES_IDX)));
    QFile stylesheet(directory.filePath(stylesheet_name));

    if (stylesheet.open(QFile::ReadOnly))
      stylesheet_contents = QString::fromUtf8(stylesheet.readAll().data());
  }

  // Define tooltips style if not already defined
  if (!stylesheet_contents.contains(QStringLiteral("QToolTip"), Qt::CaseSensitive))
  {
    const QPalette& palette = qApp->palette();
    QColor window_color;
    QColor text_color;
    QColor unused_text_emphasis_color;
    QColor border_color;
    GetToolTipStyle(window_color, text_color, unused_text_emphasis_color, border_color, palette,
                    palette);

    const auto tooltip_stylesheet =
        QStringLiteral("QToolTip { background-color: #%1; color: #%2; padding: 8px; "
                       "border: 1px; border-style: solid; border-color: #%3; }")
            .arg(window_color.rgba(), 0, 16)
            .arg(text_color.rgba(), 0, 16)
            .arg(border_color.rgba(), 0, 16);
    stylesheet_contents.append(QStringLiteral("%1").arg(tooltip_stylesheet));
  }

  qApp->setStyleSheet(stylesheet_contents);

  GetQSettings().setValue(QStringLiteral("userstyle/name"), stylesheet_name);
}

bool Settings::AreUserStylesEnabled() const
{
  return GetQSettings().value(QStringLiteral("userstyle/enabled"), false).toBool();
}

void Settings::SetUserStylesEnabled(bool enabled)
{
  GetQSettings().setValue(QStringLiteral("userstyle/enabled"), enabled);
}

void Settings::GetToolTipStyle(QColor& window_color, QColor& text_color,
                               QColor& emphasis_text_color, QColor& border_color,
                               const QPalette& palette, const QPalette& high_contrast_palette) const
{
  const auto theme_window_color = palette.color(QPalette::Base);
  const auto theme_window_hsv = theme_window_color.toHsv();
  const auto brightness = theme_window_hsv.value();
  const bool brightness_over_threshold = brightness > 128;
  const QColor emphasis_text_color_1 = Qt::yellow;
  const QColor emphasis_text_color_2 = QColor(QStringLiteral("#0090ff"));  // ~light blue
  if (Config::Get(Config::MAIN_USE_HIGH_CONTRAST_TOOLTIPS))
  {
    window_color = brightness_over_threshold ? QColor(72, 72, 72) : Qt::white;
    text_color = brightness_over_threshold ? Qt::white : Qt::black;
    emphasis_text_color = brightness_over_threshold ? emphasis_text_color_1 : emphasis_text_color_2;
    border_color = high_contrast_palette.color(QPalette::Window).darker(160);
  }
  else
  {
    window_color = palette.color(QPalette::Window);
    text_color = palette.color(QPalette::Text);
    emphasis_text_color = brightness_over_threshold ? emphasis_text_color_2 : emphasis_text_color_1;
    border_color = palette.color(QPalette::Text);
  }
}

QStringList Settings::GetPaths() const
{
  QStringList list;
  for (const auto& path : SConfig::GetInstance().m_ISOFolder)
    list << QString::fromStdString(path);
  return list;
}

void Settings::AddPath(const QString& qpath)
{
  std::string path = qpath.toStdString();

  std::vector<std::string>& paths = SConfig::GetInstance().m_ISOFolder;
  if (std::find(paths.begin(), paths.end(), path) != paths.end())
    return;

  paths.emplace_back(path);
  emit PathAdded(qpath);
}

void Settings::RemovePath(const QString& qpath)
{
  std::string path = qpath.toStdString();
  std::vector<std::string>& paths = SConfig::GetInstance().m_ISOFolder;

  auto new_end = std::remove(paths.begin(), paths.end(), path);
  if (new_end == paths.end())
    return;

  paths.erase(new_end, paths.end());
  emit PathRemoved(qpath);
}

void Settings::RefreshGameList()
{
  emit GameListRefreshRequested();
}

void Settings::NotifyRefreshGameListStarted()
{
  emit GameListRefreshStarted();
}

void Settings::NotifyRefreshGameListComplete()
{
  emit GameListRefreshCompleted();
}

void Settings::RefreshMetadata()
{
  emit MetadataRefreshRequested();
}

void Settings::NotifyMetadataRefreshComplete()
{
  emit MetadataRefreshCompleted();
}

void Settings::ReloadTitleDB()
{
  emit TitleDBReloadRequested();
}

bool Settings::IsAutoRefreshEnabled() const
{
  return GetQSettings().value(QStringLiteral("gamelist/autorefresh"), true).toBool();
}

void Settings::SetAutoRefreshEnabled(bool enabled)
{
  if (IsAutoRefreshEnabled() == enabled)
    return;

  GetQSettings().setValue(QStringLiteral("gamelist/autorefresh"), enabled);

  emit AutoRefreshToggled(enabled);
}

QString Settings::GetDefaultGame() const
{
  return QString::fromStdString(Config::Get(Config::MAIN_DEFAULT_ISO));
}

void Settings::SetDefaultGame(QString path)
{
  if (GetDefaultGame() != path)
  {
    Config::SetBase(Config::MAIN_DEFAULT_ISO, path.toStdString());
    emit DefaultGameChanged(path);
  }
}

bool Settings::GetPreferredView() const
{
  return GetQSettings().value(QStringLiteral("PreferredView"), true).toBool();
}

void Settings::SetPreferredView(bool list)
{
  GetQSettings().setValue(QStringLiteral("PreferredView"), list);
}

int Settings::GetStateSlot() const
{
  return GetQSettings().value(QStringLiteral("Emulation/StateSlot"), 1).toInt();
}

void Settings::SetStateSlot(int slot)
{
  GetQSettings().setValue(QStringLiteral("Emulation/StateSlot"), slot);
}

void Settings::SetHideCursor(bool hide_cursor)
{
  SConfig::GetInstance().bHideCursor = hide_cursor;
  emit HideCursorChanged();
}

bool Settings::GetHideCursor() const
{
  return SConfig::GetInstance().bHideCursor;
}

void Settings::SetLockCursor(bool lock_cursor)
{
  SConfig::GetInstance().bLockCursor = lock_cursor;
  emit LockCursorChanged();
}

bool Settings::GetLockCursor() const
{
  return SConfig::GetInstance().bLockCursor;
}

void Settings::SetKeepWindowOnTop(bool top)
{
  if (IsKeepWindowOnTopEnabled() == top)
    return;

  Config::SetBaseOrCurrent(Config::MAIN_KEEP_WINDOW_ON_TOP, top);
  emit KeepWindowOnTopChanged(top);
}

bool Settings::IsKeepWindowOnTopEnabled() const
{
  return Config::Get(Config::MAIN_KEEP_WINDOW_ON_TOP);
}

int Settings::GetVolume() const
{
  return SConfig::GetInstance().m_Volume;
}

void Settings::SetVolume(int volume)
{
  if (GetVolume() != volume)
  {
    SConfig::GetInstance().m_Volume = volume;
    emit VolumeChanged(volume);
  }
}

void Settings::IncreaseVolume(int volume)
{
  AudioCommon::IncreaseVolume(volume);
  emit VolumeChanged(GetVolume());
}

void Settings::DecreaseVolume(int volume)
{
  AudioCommon::DecreaseVolume(volume);
  emit VolumeChanged(GetVolume());
}

bool Settings::IsLogVisible() const
{
  return GetQSettings().value(QStringLiteral("logging/logvisible")).toBool();
}

void Settings::SetLogVisible(bool visible)
{
  if (IsLogVisible() != visible)
  {
    GetQSettings().setValue(QStringLiteral("logging/logvisible"), visible);
    emit LogVisibilityChanged(visible);
  }
}

bool Settings::IsLogConfigVisible() const
{
  return GetQSettings().value(QStringLiteral("logging/logconfigvisible")).toBool();
}

void Settings::SetLogConfigVisible(bool visible)
{
  if (IsLogConfigVisible() != visible)
  {
    GetQSettings().setValue(QStringLiteral("logging/logconfigvisible"), visible);
    emit LogConfigVisibilityChanged(visible);
  }
}

std::shared_ptr<NetPlay::NetPlayClient> Settings::GetNetPlayClient()
{
  return m_client;
}

void Settings::ResetNetPlayClient(NetPlay::NetPlayClient* client)
{
  m_client.reset(client);

  g_netplay_chat_ui.reset();
  g_netplay_golf_ui.reset();
}

std::shared_ptr<NetPlay::NetPlayServer> Settings::GetNetPlayServer()
{
  return m_server;
}

void Settings::ResetNetPlayServer(NetPlay::NetPlayServer* server)
{
  m_server.reset(server);
}

bool Settings::GetCheatsEnabled() const
{
  return SConfig::GetInstance().bEnableCheats;
}

void Settings::SetCheatsEnabled(bool enabled)
{
  if (SConfig::GetInstance().bEnableCheats != enabled)
  {
    SConfig::GetInstance().bEnableCheats = enabled;
    emit EnableCheatsChanged(enabled);
  }
}

void Settings::SetDebugModeEnabled(bool enabled)
{
  if (IsDebugModeEnabled() != enabled)
  {
    SConfig::GetInstance().bEnableDebugging = enabled;
    emit DebugModeToggled(enabled);
  }
  if (enabled)
    SetCodeVisible(true);
}

bool Settings::IsDebugModeEnabled() const
{
  return SConfig::GetInstance().bEnableDebugging;
}

void Settings::SetRegistersVisible(bool enabled)
{
  if (IsRegistersVisible() != enabled)
  {
    GetQSettings().setValue(QStringLiteral("debugger/showregisters"), enabled);

    emit RegistersVisibilityChanged(enabled);
  }
}

bool Settings::IsThreadsVisible() const
{
  return GetQSettings().value(QStringLiteral("debugger/showthreads")).toBool();
}

void Settings::SetThreadsVisible(bool enabled)
{
  if (IsThreadsVisible() == enabled)
    return;

  GetQSettings().setValue(QStringLiteral("debugger/showthreads"), enabled);
  emit ThreadsVisibilityChanged(enabled);
}

bool Settings::IsRegistersVisible() const
{
  return GetQSettings().value(QStringLiteral("debugger/showregisters")).toBool();
}

void Settings::SetWatchVisible(bool enabled)
{
  if (IsWatchVisible() != enabled)
  {
    GetQSettings().setValue(QStringLiteral("debugger/showwatch"), enabled);

    emit WatchVisibilityChanged(enabled);
  }
}

bool Settings::IsWatchVisible() const
{
  return GetQSettings().value(QStringLiteral("debugger/showwatch")).toBool();
}

void Settings::SetBreakpointsVisible(bool enabled)
{
  if (IsBreakpointsVisible() != enabled)
  {
    GetQSettings().setValue(QStringLiteral("debugger/showbreakpoints"), enabled);

    emit BreakpointsVisibilityChanged(enabled);
  }
}

bool Settings::IsBreakpointsVisible() const
{
  return GetQSettings().value(QStringLiteral("debugger/showbreakpoints")).toBool();
}

void Settings::SetCodeVisible(bool enabled)
{
  if (IsCodeVisible() != enabled)
  {
    GetQSettings().setValue(QStringLiteral("debugger/showcode"), enabled);

    emit CodeVisibilityChanged(enabled);
  }
}

bool Settings::IsCodeVisible() const
{
  return GetQSettings().value(QStringLiteral("debugger/showcode")).toBool();
}

void Settings::SetMemoryVisible(bool enabled)
{
  if (IsMemoryVisible() == enabled)
    return;
  QSettings().setValue(QStringLiteral("debugger/showmemory"), enabled);

  emit MemoryVisibilityChanged(enabled);
}

bool Settings::IsMemoryVisible() const
{
  return QSettings().value(QStringLiteral("debugger/showmemory")).toBool();
}

void Settings::SetNetworkVisible(bool enabled)
{
  if (IsNetworkVisible() == enabled)
    return;

  GetQSettings().setValue(QStringLiteral("debugger/shownetwork"), enabled);
  emit NetworkVisibilityChanged(enabled);
}

bool Settings::IsNetworkVisible() const
{
  return GetQSettings().value(QStringLiteral("debugger/shownetwork")).toBool();
}

void Settings::SetJITVisible(bool enabled)
{
  if (IsJITVisible() == enabled)
    return;
  QSettings().setValue(QStringLiteral("debugger/showjit"), enabled);

  emit JITVisibilityChanged(enabled);
}

bool Settings::IsJITVisible() const
{
  return QSettings().value(QStringLiteral("debugger/showjit")).toBool();
}

void Settings::RefreshWidgetVisibility()
{
  emit DebugModeToggled(IsDebugModeEnabled());
  emit LogVisibilityChanged(IsLogVisible());
  emit LogConfigVisibilityChanged(IsLogConfigVisible());
}

void Settings::SetDebugFont(QFont font)
{
  if (GetDebugFont() != font)
  {
    GetQSettings().setValue(QStringLiteral("debugger/font"), font);

    emit DebugFontChanged(font);
  }
}

QFont Settings::GetDebugFont() const
{
  QFont default_font = QFont(QFontDatabase::systemFont(QFontDatabase::FixedFont).family());

  return GetQSettings().value(QStringLiteral("debugger/font"), default_font).value<QFont>();
}

void Settings::SetAutoUpdateTrack(const QString& mode)
{
  if (mode == GetAutoUpdateTrack())
    return;

  SConfig::GetInstance().m_auto_update_track = mode.toStdString();

  emit AutoUpdateTrackChanged(mode);
}

QString Settings::GetAutoUpdateTrack() const
{
  return QString::fromStdString(SConfig::GetInstance().m_auto_update_track);
}

void Settings::SetFallbackRegion(const DiscIO::Region& region)
{
  if (region == GetFallbackRegion())
    return;

  Config::SetBase(Config::MAIN_FALLBACK_REGION, region);

  emit FallbackRegionChanged(region);
}

DiscIO::Region Settings::GetFallbackRegion() const
{
  return Config::Get(Config::MAIN_FALLBACK_REGION);
}

void Settings::SetAnalyticsEnabled(bool enabled)
{
  if (enabled == IsAnalyticsEnabled())
    return;

  Config::SetBase(Config::MAIN_ANALYTICS_ENABLED, enabled);

  emit AnalyticsToggled(enabled);
}

bool Settings::IsAnalyticsEnabled() const
{
  return Config::Get(Config::MAIN_ANALYTICS_ENABLED);
}

void Settings::SetToolBarVisible(bool visible)
{
  if (IsToolBarVisible() == visible)
    return;

  GetQSettings().setValue(QStringLiteral("toolbar/visible"), visible);

  emit ToolBarVisibilityChanged(visible);
}

bool Settings::IsToolBarVisible() const
{
  return GetQSettings().value(QStringLiteral("toolbar/visible"), true).toBool();
}

void Settings::SetWidgetsLocked(bool locked)
{
  if (AreWidgetsLocked() == locked)
    return;

  GetQSettings().setValue(QStringLiteral("widgets/locked"), locked);

  emit WidgetLockChanged(locked);
}

bool Settings::AreWidgetsLocked() const
{
  return GetQSettings().value(QStringLiteral("widgets/locked"), true).toBool();
}

bool Settings::IsBatchModeEnabled() const
{
  return m_batch;
}
void Settings::SetBatchModeEnabled(bool batch)
{
  m_batch = batch;
}

bool Settings::IsSDCardInserted() const
{
  return SConfig::GetInstance().m_WiiSDCard;
}

void Settings::SetSDCardInserted(bool inserted)
{
  if (IsSDCardInserted() != inserted)
  {
    SConfig::GetInstance().m_WiiSDCard = inserted;
    emit SDCardInsertionChanged(inserted);

    auto* ios = IOS::HLE::GetIOS();
    if (ios)
      ios->SDIO_EventNotify();
  }
}

bool Settings::IsUSBKeyboardConnected() const
{
  return SConfig::GetInstance().m_WiiKeyboard;
}

void Settings::SetUSBKeyboardConnected(bool connected)
{
  if (IsUSBKeyboardConnected() != connected)
  {
    SConfig::GetInstance().m_WiiKeyboard = connected;
    emit USBKeyboardConnectionChanged(connected);
  }
}
