#pragma once

#include <QMap>
#include <QString>

// Name of the GPU of each vendor in this PC, e.g. {"nvidia": "NVIDIA GeForce RTX 3080"}.
QMap<QString, QString> gpuNames();

QString cpuName();
