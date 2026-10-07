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

// Name of an ARM core design from the ids in /proc/cpuinfo, e.g. ("0x41", "0xd0b") -> "Cortex-A76"
QString armCoreName(const QString &implementer, const QString &part)
{
    static const QMap<QString, QString> arm = {
        {"0xd03", "Cortex-A53"}, {"0xd04", "Cortex-A35"}, {"0xd05", "Cortex-A55"}, {"0xd07", "Cortex-A57"},
        {"0xd08", "Cortex-A72"}, {"0xd09", "Cortex-A73"}, {"0xd0a", "Cortex-A75"}, {"0xd0b", "Cortex-A76"},
        {"0xd0c", "Neoverse-N1"}, {"0xd0d", "Cortex-A77"}, {"0xd40", "Neoverse-V1"}, {"0xd41", "Cortex-A78"},
        {"0xd44", "Cortex-X1"}, {"0xd46", "Cortex-A510"}, {"0xd47", "Cortex-A710"}, {"0xd48", "Cortex-X2"},
        {"0xd49", "Neoverse-N2"}, {"0xd4b", "Cortex-A78C"}, {"0xd4d", "Cortex-A715"}, {"0xd4e", "Cortex-X3"},
        {"0xd4f", "Neoverse-V2"}, {"0xd80", "Cortex-A520"}, {"0xd81", "Cortex-A720"}, {"0xd82", "Cortex-X4"},
        {"0xd84", "Neoverse-V3"}, {"0xd85", "Cortex-X925"}, {"0xd87", "Cortex-A725"}, {"0xd8e", "Neoverse-N3"}};
    const QString id = part.toLower();
    if (implementer == "0x41" && arm.contains(id))
        return "ARM " + arm.value(id);
    if (implementer == "0x51")
        return id == "0x001" ? QStringLiteral("Qualcomm Oryon") : QStringLiteral("Qualcomm");
    if (implementer == "0x61")
        return QStringLiteral("Apple Silicon");   // Asahi Linux
    return QStringLiteral("ARM");
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
    QStringList cores;   // ARM: no model name, only the ids of the core designs (big.LITTLE: more than one)
    QString implementer;
    QFile cpuinfo("/proc/cpuinfo");
    if (cpuinfo.open(QIODevice::ReadOnly))
        for (const QByteArray &line : cpuinfo.readAll().split('\n')) {
            const QString value = QString::fromUtf8(line.mid(line.indexOf(':') + 1)).simplified();
            if (line.startsWith("model name") && name.isEmpty())
                name = value;
            else if (line.startsWith("CPU implementer"))
                implementer = value;
            else if (line.startsWith("CPU part")) {
                const QString core = armCoreName(implementer, value);
                if (!cores.contains(core))
                    cores.prepend(core);   // the big cores are usually listed last
            }
        }
    if (name.isEmpty() && !cores.isEmpty()) {
        name = cores.join(" + ");
        // boards like the Raspberry Pi have their name in the device tree
        const QString board = readFirstLine("/sys/firmware/devicetree/base/model").remove(QChar(0)).trimmed();
        if (!board.isEmpty())
            name = board + " (" + name + ")";
    }
#else
    const QString name;
#endif
    return name.isEmpty() ? QStringLiteral("CPU") : name;
}
