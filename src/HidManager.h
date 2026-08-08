#pragma once

// HidManager - 平台 HID 门面, MainWindow 仅依赖此名.
//
// 各平台具体实现:
//   macOS : HidManagerMac  (纯 IOHIDManager, 见 HidManagerMac.h 头注释)
//   Windows: HidManagerWin (原生 Win32 HID, SetupAPI + ReadFile/WriteFile)
// 二者均实现 ITransport, 并提供静态 enumerate(vid,pid) -> QList<HidDeviceInfo>.
// 公共接口完全一致, MainWindow 不感知平台.

#include <QtCore/QtGlobal>

#ifdef Q_OS_MACOS
  #include "HidManagerMac.h"
  using HidManager = HidManagerMac;
#elif defined(Q_OS_WIN)
  #include "HidManagerWin.h"
  using HidManager = HidManagerWin;
#endif
