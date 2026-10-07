#include "systeminfo.h"

#include "codecs.h"

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTextStream>

#if defined(Q_OS_WIN)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <sys/sysctl.h>
#endif

namespace {

#if defined(Q_OS_MACOS)
QString sysctlString(const char *name)
{
    size_t size = 0;
    if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0)
        return {};
    QByteArray value(qsizetype(size), '\0');
    if (sysctlbyname(name, value.data(), &size, nullptr, 0) != 0)
        return {};
    return QString::fromUtf8(value.constData()).simplified();
}
#endif

#if defined(Q_OS_LINUX)
QString readFirstLine(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readLine()).trimmed();
}

// Device name from the PCI ID database, e.g. "Navi 21 [Radeon RX 6800/6800 XT / 6900 XT]"
QString pciDeviceName(const QString &vendorId, const QString &deviceId)
{
    for (const QString &path : {"/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids"}) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        QTextStream in(&file);
        bool inVendor = false;
        while (!in.atEnd()) {
            const QString line = in.readLine();
            if (line.isEmpty() || line.startsWith('#'))
                continue;
            if (!line.startsWith('\t')) {
                if (inVendor)
                    break;
                inVendor = line.startsWith(vendorId, Qt::CaseInsensitive);
            } else if (inVendor && !line.startsWith("\t\t") && line.mid(1, 4).compare(deviceId, Qt::CaseInsensitive) == 0) {
                return line.mid(5).trimmed();
            }
        }
    }
    return {};
}
#endif

} // namespace

QMap<QString, QString> gpuNames()
{
    QMap<QString, QString> names;
#if defined(Q_OS_WIN)
    DISPLAY_DEVICEW dev;
    for (DWORD i = 0;; ++i) {
        dev = {};
        dev.cb = sizeof(dev);
        if (!EnumDisplayDevicesW(nullptr, i, &dev, 0))
            break;
        const QString id = QString::fromWCharArray(dev.DeviceID).toUpper();
        for (const Vendor &vendor : vendors())   // virtual adapters (no PCI id) are skipped
            if (!vendor.pciId.isEmpty() && id.contains("VEN_" + vendor.pciId) && !names.contains(vendor.key))
                names.insert(vendor.key, QString::fromWCharArray(dev.DeviceString));
    }
#elif defined(Q_OS_MACOS)
    // Apple Silicon: the GPU and the video encoders are part of the chip, e.g. "Apple M2 Pro"
    const QString chip = sysctlString("machdep.cpu.brand_string");
    if (chip.startsWith("Apple"))
        names.insert("apple", chip);
#elif defined(Q_OS_LINUX)
    // the NVIDIA driver publishes the model name
    for (const QString &gpu : QDir("/proc/driver/nvidia/gpus").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile info("/proc/driver/nvidia/gpus/" + gpu + "/information");
        if (info.open(QIODevice::ReadOnly))
            for (const QByteArray &line : info.readAll().split('\n'))
                if (line.startsWith("Model:") && !names.contains("nvidia"))
                    names.insert("nvidia", QString::fromUtf8(line.mid(6)).trimmed());
    }
    // other GPUs: vendor and device id from sysfs, name from the PCI ID database
    for (const QString &card : QDir("/sys/class/drm").entryList({"card?", "card??"}, QDir::Dirs | QDir::System)) {
        const QString device = "/sys/class/drm/" + card + "/device/";
        const QString vendorId = readFirstLine(device + "vendor").remove("0x").toUpper();
        const QString deviceId = readFirstLine(device + "device").remove("0x").toUpper();
        for (const Vendor &vendor : vendors())
            if (!vendor.pciId.isEmpty() && vendorId == vendor.pciId && !names.contains(vendor.key)) {
                const QString name = pciDeviceName(vendorId.toLower(), deviceId.toLower());
                if (!name.isEmpty())
                    names.insert(vendor.key, name);
            }
    }
#endif
    return names;
}

QString cpuName()
{
#if defined(Q_OS_WIN)
    const QSettings key(R"(HKEY_LOCAL_MACHINE\HARDWARE\DESCRIPTION\System\CentralProcessor\0)",
                        QSettings::NativeFormat);
    const QString name = key.value("ProcessorNameString").toString().simplified();
#elif defined(Q_OS_MACOS)
    const QString name = sysctlString("machdep.cpu.brand_string");
#elif defined(Q_OS_LINUX)
    QString name;
    QFile cpuinfo("/proc/cpuinfo");
    if (cpuinfo.open(QIODevice::ReadOnly))
        for (const QByteArray &line : cpuinfo.readAll().split('\n'))
            if (line.startsWith("model name")) {
                name = QString::fromUtf8(line.mid(line.indexOf(':') + 1)).simplified();
                break;
            }
#else
    const QString name;
#endif
    return name.isEmpty() ? QStringLiteral("CPU") : name;
}
