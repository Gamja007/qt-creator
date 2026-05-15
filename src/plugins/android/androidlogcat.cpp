// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "androidlogcat.h"

#include "androidconfigurations.h"
#include "androidconstants.h"
#include "androiddevice.h"
#include "androidtr.h"
#include "androidutils.h"

#include <coreplugin/actionmanager/actioncontainer.h>
#include <coreplugin/actionmanager/actionmanager.h>
#include <coreplugin/outputwindow.h>

#include <projectexplorer/devicesupport/devicemanager.h>
#include <projectexplorer/devicesupport/idevice.h>
#include <projectexplorer/projectexplorerconstants.h>
#include <projectexplorer/runcontrol.h>

#include <utils/commandline.h>
#include <utils/outputformat.h>
#include <utils/qtcprocess.h>

#include <QtCore/qchar.h>

#include <QtTaskTree/QBarrier>
#include <QtTaskTree/QTaskTree>
#include <QtTaskTree/qtasktree.h>

#include <QHash>
#include <QMenu>
#include <QObject>
#include <QPointer>

#include <memory>

using namespace Utils;
using namespace Core;
using namespace QtTaskTree;
using namespace ProjectExplorer;

namespace Android::Internal {

using AndroidDeviceConstPtr = std::shared_ptr<const AndroidDevice>;

//Helpers

static CommandLine adbLogcat(const QString &serialNumber, const QStringList &extra = {})
{
    return {AndroidConfig::adbToolPath(), adbSelector(serialNumber) + QStringList{"logcat"} + extra};
}

struct LogcatEntry
{
    QString line; //"(pid)" already stripped
    Utils::OutputFormat fmt;
    QChar level;
    qint32 pid = -1;
    bool bypassFilter = false;
};

static const QRegularExpression regExpLogcat(
    "^"                     // line start
    "(?:\\x1b\\[[0-9;]*m)?" // optional ANSI color
    "([VDIWEF])"            // log level
    "/"                     // level/tag separator
    "([^(]*)"               // tag
    "\\(\\s*(\\d+)\\s*\\)"  // PID
);

// Parse a raw logcat line into a LogcatEntry, stripping the "(pid)" segment
// so the stored line reads "level/tag: message".
static LogcatEntry parseLogcat(const QString &raw)
{
    LogcatEntry e{.line = raw};
    const auto match = regExpLogcat.match(raw);
    if (match.hasMatch()) {
        e.level = match.captured(1).at(0);
        bool ok = false;
        if (const int pid = match.captured(3).toInt(&ok); ok)
            e.pid = pid;
        const int from = e.line.indexOf(QLatin1Char('('));
        const int to = e.line.indexOf(QLatin1Char(')'), from);
        if (from >= 0 && to > from)
            e.line.remove(from, to - from + 1);
    }
    const bool isError = e.level == QLatin1Char('W') || e.level == QLatin1Char('E')
                         || e.level == QLatin1Char('F');
    e.fmt = isError ? Utils::StdErrFormat : Utils::StdOutFormat;
    return e;
}

static QString logcatTitle(const QString &label)
{
    return Tr::tr("Logcat (%1)").arg(label);
}

// User-facing title for a device: "<displayName> - <serial>". Shared by
// the Tools > Android > Logcat submenu entries and the Logcat tab title.
static QString deviceTitle(const AndroidDeviceConstPtr &device)
{
    return QString("%1 - %2").arg(device->displayName(), device->serialNumber());
}

static QString banner(const QString &label, const QString &state)
{
    return QString("**** %1 - %2 ****").arg(label, state);
}

static AndroidDeviceConstPtr asReadyAndroidDevice(const IDeviceConstPtr &device)
{
    if (!device || device->type() != Constants::ANDROID_DEVICE_TYPE
        || device->deviceState() != IDevice::DeviceReadyToUse)
        return {};
    return std::dynamic_pointer_cast<const AndroidDevice>(device);
}

//LogcatFilter
class LogcatFilter
{
public:
    void setFromText(const QString &text);
    bool accepts(const LogcatEntry &entry) const;

    QString cachedText() const { return m_cachedText; }
    bool isActive() const { return !m_predicates.isEmpty(); }

    using FilterPredicate = std::function<bool(const LogcatEntry &)>;

private:
    QList<FilterPredicate> m_predicates;
    // this is going to be used as package:com.domain.package in upcoming commits.
    // To indicate that the application is bound with this
    QString m_cachedText;
};

void LogcatFilter::setFromText(const QString &text)
{
    m_cachedText = text;
    m_predicates.clear();
}

bool LogcatFilter::accepts(const LogcatEntry &entry) const
{
    if (entry.bypassFilter)
        return true;
    for (const FilterPredicate &filterPredicate : m_predicates)
        if (!filterPredicate(entry))
            return false;
    return true;
}

//LogcatStream
class LogcatStream : public QObject
{
public:
    LogcatStream(AndroidDeviceConstPtr device);
    ~LogcatStream() override { stop(); }
    Id deviceId() const { return m_device->id(); }
    QString serial() const { return m_device->serialNumber(); }
    QString tabLabel() const;

    void start();
    void stop();

    RunControl *tab() const { return m_tabContext.tab; }
    void setTab(RunControl *tab);
    void setTabActive(bool active);

    void postMessage(const QString &msg, Utils::OutputFormat fmt);

private:
    struct TabContext
    {
        QPointer<RunControl> tab;
        bool active = false;
        QList<LogcatEntry> buffer;
        LogcatFilter filter;

        void appendEntry(const LogcatEntry &entry);
        void applyFilter() const;
        void renderFromBuffer() const;

        QString windowFilterText() const
        {
            return filter.isActive() ? QString() : filter.cachedText();
        }
    };
    enum class Lifecycle { Stop, Start };

    void startAdbTail();
    void stopAdbTail();

    void parseLine(const QString &raw);

    void onDisconnected();
    void onConnected();

    void onOutputFilterTextChanged(const QString &text);

    const AndroidDeviceConstPtr m_device;
    std::unique_ptr<QTaskTree> m_task;
    TabContext m_tabContext;
    Lifecycle m_lifecycle = Lifecycle::Stop;
};

static QHash<Id, LogcatStream *> &streamRegistry()
{
    static QHash<Id, LogcatStream *> map;
    return map;
}

LogcatStream::LogcatStream(AndroidDeviceConstPtr device)
    : m_device(std::move(device))
{
    DeviceManager *dm = DeviceManager::instance();
    QObject::connect(dm, &DeviceManager::deviceRemoved, this, [this](Id removedId) {
        if (removedId == deviceId())
            onDisconnected();
    });
    connect(dm, &DeviceManager::deviceUpdated, this, [this](Id id) {
        if (id != deviceId())
            return;
        const auto state = m_device->deviceState();
        if (state == IDevice::DeviceDisconnected)
            onDisconnected();
        else if (state == IDevice::DeviceReadyToUse)
            onConnected();
    });
}

QString LogcatStream::tabLabel() const
{
    return deviceTitle(m_device);
}

void LogcatStream::setTab(RunControl *tab)
{
    m_tabContext = {};
    m_tabContext.tab = tab;

    if (!tab) {
        const Id id = deviceId();
        stop();
        streamRegistry().remove(id);
        return;
    }
    // adb tails only while the tab is the currently selected one
    QObject::connect(tab, &RunControl::tabActiveChanged, this, [this](bool active) {
        setTabActive(active);
    });

    QObject::connect(tab, &RunControl::outputFilterChanged, this, [this](const QString &text) {
        onOutputFilterTextChanged(text);
    });

    QObject::connect(tab, &QObject::destroyed, this, [this] {
        setTab(nullptr);
        deleteLater();
    });
}

void LogcatStream::setTabActive(bool active)
{
    if (!m_tabContext.tab)
        return;
    if (active == m_tabContext.active)
        return;
    m_tabContext.active = active;
    if (active)
        start();
    else
        stop();
}

void LogcatStream::start()
{
    if (m_lifecycle == Lifecycle::Start)
        return;
    if (m_device->deviceState() != IDevice::DeviceReadyToUse)
        return;
    startAdbTail();
    m_lifecycle = Lifecycle::Start;
}

void LogcatStream::stop()
{
    if (m_lifecycle == Lifecycle::Stop)
        return;
    stopAdbTail();
    m_lifecycle = Lifecycle::Stop;
}

void LogcatStream::onDisconnected()
{
    if (m_lifecycle == Lifecycle::Stop)
        return;
    postMessage(banner(tabLabel(), QLatin1String("disconnected")), Utils::NormalMessageFormat);
    stop();
}

void LogcatStream::onConnected()
{
    if (m_tabContext.tab && m_tabContext.active)
        start();
}

void LogcatStream::postMessage(const QString &msg, Utils::OutputFormat fmt)
{
    m_tabContext.appendEntry({.line = msg, .fmt = fmt, .bypassFilter = true});
}

void LogcatStream::TabContext::appendEntry(const LogcatEntry &entry)
{
    static constexpr int kMaxBufferedLines = 10000;
    buffer.append(entry);
    if (buffer.size() > kMaxBufferedLines)
        buffer.removeFirst();
    if (tab && filter.accepts(entry))
        tab->postMessage(entry.line, entry.fmt, false);
}

void LogcatStream::TabContext::applyFilter() const
{
    if (!tab)
        return;
    tab->setOutputFilterText(filter.cachedText());
    if (OutputWindow *const w = tab->outputWindow())
        w->updateFilterProperties(windowFilterText(), Qt::CaseInsensitive, false, false, 0, 0);
}

void LogcatStream::TabContext::renderFromBuffer() const
{
    applyFilter();
    OutputWindow *const w = tab ? tab->outputWindow() : nullptr;
    if (!w)
        return;
    w->clear();
    for (const LogcatEntry &entry : buffer) {
        if (filter.accepts(entry))
            tab->postMessage(entry.line, entry.fmt, false);
    }
}

void LogcatStream::onOutputFilterTextChanged(const QString &text)
{
    m_tabContext.filter.setFromText(text);
    m_tabContext.renderFromBuffer();
}

void LogcatStream::startAdbTail()
{
    const QString serialNumber = serial();
    const auto onSetup = [this, serialNumber](Process &process) {
        process.setStdOutLineCallback([this](const QString &line) { parseLine(line); });
        process.setStdErrLineCallback([this](const QString &line) {
            if (m_tabContext.tab)
                m_tabContext.tab->postMessage(line, Utils::StdErrFormat, false);
        });
        // -T 1 starts the tail at the current head, skipping the device's
        // existing ring buffer (live tail only), no destructive 'logcat -c'.
        process.setCommand(adbLogcat(serialNumber, {"-T", "1", "-v", "color", "-v", "brief"}));
    };
    m_task = std::make_unique<QTaskTree>(Group{Forever{ProcessTask(onSetup) || successItem}});
    m_task->start();
}

void LogcatStream::stopAdbTail()
{
    if (m_task)
        m_task.reset();
}

void LogcatStream::parseLine(const QString &raw)
{
    m_tabContext.appendEntry(parseLogcat(raw));
}

static LogcatStream *ensureStream(const AndroidDeviceConstPtr &device)
{
    if (!device || device->serialNumber().isEmpty())
        return nullptr;
    const auto id = device->id();
    auto &reg = streamRegistry();
    if (auto *stream = reg.value(id))
        return stream;
    auto *stream = new LogcatStream(device);
    reg.insert(id, stream);
    return stream;
}

//Tab plumbing

static RunControl *openLogcatTabForStream(LogcatStream *logcatStream)
{
    if (!logcatStream)
        return nullptr;
    if (RunControl *existing = logcatStream->tab())
        return existing;
    auto *runControl = new RunControl(ProjectExplorer::Constants::NORMAL_RUN_MODE);
    runControl->setDisplayName(logcatTitle(logcatStream->tabLabel()));
    runControl->setPromptToStop([](bool *) { return true; });
    logcatStream->setTab(runControl);

    QPointer<RunControl> rcPtr = runControl;
    rcPtr->setRunRecipe(QBarrierTask([](QBarrier &) {}).withCancel([rcPtr] {
        return makeObjectSignal(rcPtr.data(), &RunControl::canceled);
    }));
    rcPtr->start();
    return rcPtr;
}

static RunControl *ensureVisibleTab(const AndroidDeviceConstPtr &device)
{
    auto *stream = ensureStream(device);
    if (!stream)
        return nullptr;
    return openLogcatTabForStream(stream);
}

//Menu wiring

// Tools > Android > Logcat is a submenu listing every ready Android device.
// rebuilt on demand so connected/disconnected transitions show up immediately.
static void populateLogcatSubmenu(QMenu *menu)
{
    qDeleteAll(menu->actions());
    DeviceManager::forEachDevice([menu](const IDeviceConstPtr &device) {
        const auto androidDev = asReadyAndroidDevice(device);
        if (!androidDev)
            return;
        menu->addAction(deviceTitle(androidDev), menu, [androidDev] { ensureVisibleTab(androidDev); });
    });
    if (menu->isEmpty()) {
        QAction *const placeholder = menu->addAction(Tr::tr("No Android device connected"));
        placeholder->setEnabled(false);
    }
}

//Public API

void initAndroidLogcat()
{
    ActionContainer *const logcatMenu = ActionManager::createMenu("Android.Tools.Logcat");
    logcatMenu->menu()->setTitle(Tr::tr("Logcat"));
    logcatMenu->setOnAllDisabledBehavior(ActionContainer::Show);

    ActionContainer *const parent = ActionManager::actionContainer(Constants::ANDROID_TOOLS_MENU_ID);
    if (!parent)
        return;
    parent->addMenu(logcatMenu);
    QObject::connect(parent->menu(), &QMenu::aboutToShow, parent->menu(), [logcatMenu] {
        populateLogcatSubmenu(logcatMenu->menu());
    });
}

} // namespace Android::Internal
