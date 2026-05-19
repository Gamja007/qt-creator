// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

namespace ProjectExplorer {
class RunControl;
}

namespace Android::Internal {
// Install the Tools > Android > Logcat submenu. Triggering an entry opens
// (or raises) the Logcat tab for the corresponding ready Android device.
void initAndroidLogcat();
void bindRunningAppToLogcat(
    ProjectExplorer::RunControl *runControl, qint64 pid, const QString &packageName);
void unbindRunningAppFromLogcat(ProjectExplorer::RunControl *runControl);
} // namespace Android::Internal
