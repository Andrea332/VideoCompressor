#include "systeminfo.h"

#include "codecs.h"

#include <QSettings>

#ifdef Q_OS_WIN
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

QMap<QString, QString> gpuNames()
{
    QMap<QString, QString> names;
#ifdef Q_OS_WIN
    DISPLAY_DEVICEW dev;
    for (DWORD i = 0;; ++i) {
        dev = {};
        dev.cb = sizeof(dev);
        if (!EnumDisplayDevicesW(nullptr, i, &dev, 0))
            break;
        const QString id = QString::fromWCharArray(dev.DeviceID).toUpper();
        for (const Vendor &vendor : vendors())   // virtual adapters (no PCI id) are skipped
            if (id.contains("VEN_" + vendor.pciId) && !names.contains(vendor.key))
                names.insert(vendor.key, QString::fromWCharArray(dev.DeviceString));
    }
#endif
    return names;
}

QString cpuName()
{
#ifdef Q_OS_WIN
    const QSettings key(R"(HKEY_LOCAL_MACHINE\HARDWARE\DESCRIPTION\System\CentralProcessor\0)",
                        QSettings::NativeFormat);
    const QString name = key.value("ProcessorNameString").toString().simplified();
    if (!name.isEmpty())
        return name;
#endif
    return QStringLiteral("CPU");
}
