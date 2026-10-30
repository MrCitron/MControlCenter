/* Copyright (C) 2022  Dmitry Serov
 *
 * This file is part of MControlCenter.
 *
 * MControlCenter is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * MControlCenter is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MControlCenter. If not, see <https://www.gnu.org/licenses/>.
 */

#include "helper.h"
#include "msi-ec.h"
#include "readwrite.h"
#include "wmifancurve.h"
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QFile>
#include <QProcess>
#include <QStringList>
#include <QTimer>

ReadWrite rw;

void Helper::quit() const {
    QTimer::singleShot(0, QCoreApplication::instance(), &QCoreApplication::quit);
}

QByteArray Helper::getData() const {
    return rw.readFromFile();
}

void Helper::putValue(const int &address, const int &value) const {
    if (value >= 0 && value <= 255)
        rw.writeToFile(address, value);
    else
        fprintf(stderr, "tried to input invalid value. Address: %d, value: %d\n", address, value);
}

bool Helper::setWmiFanCurve(const QString &attribute, const QList<int> &value) const {
    // Strict validation: only the two known duty-curve attributes of this
    // exact WMI device, and exactly 7 duty values inside the EC byte range.
    // The kernel driver additionally refuses the write unless it was loaded
    // with allow_curve_writes=1 and verifies each write with a readback.
    if (attribute != WMI_FAN_DUTY_CURVE_FAN1 && attribute != WMI_FAN_DUTY_CURVE_FAN2) {
        fprintf(stderr, "SetWmiFanCurve: unexpected attribute %s\n", qPrintable(attribute));
        return false;
    }
    if (value.size() != WMI_FAN_DUTY_CURVE_COUNT) {
        fprintf(stderr, "SetWmiFanCurve: exactly %d duty values expected, got %d\n",
                WMI_FAN_DUTY_CURVE_COUNT, static_cast<int>(value.size()));
        return false;
    }
    for (int v : value) {
        if (v < 0 || v > 255) {
            fprintf(stderr, "SetWmiFanCurve: invalid duty value %d\n", v);
            return false;
        }
    }

    QFile file(QString(WMI_PLATFORM_DEVICE_PATH) + '/' + attribute);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        fprintf(stderr, "SetWmiFanCurve %s: cannot open attribute\n", qPrintable(attribute));
        return false;
    }
    QStringList tokens;
    tokens.reserve(value.size());
    for (int v : value)
        tokens.push_back(QString::number(v));
    const QByteArray payload = tokens.join(QLatin1Char(' ')).toUtf8() + '\n';
    if (file.write(payload) != payload.size()) {
        fprintf(stderr, "SetWmiFanCurve %s: rejected by the driver (%s)\n",
                qPrintable(attribute), qPrintable(file.errorString()));
        return false;
    }
    return true;
}

bool Helper::isEcSysModuleLoaded() const {
    if (rw.isEcSys()) {
        return true;
    }
    if (rw.isAcpiEc()) {
        fprintf(stderr, "%s\n", qPrintable("The acpi_ec kernel module is loaded"));
        return true;
    }
    fprintf(stderr, "%s\n", qPrintable("The ec_sys kernel module is not loaded"));
    return false;
}

bool Helper::loadEcSysModule() const {
    fprintf(stderr, "%s\n", qPrintable("Trying to load the ec_sys kernel module"));
    auto *process = new QProcess();
    process->start("sh", QStringList() << "-c" << "/usr/sbin/modprobe ec_sys write_support=1 2>&1");
    process->waitForFinished(1000);
    if (QByteArray output = process->readAllStandardOutput(); output != "")
        fprintf(stderr, "%s", qPrintable(output));
    if (isEcSysModuleLoaded())
        return true;
    return false;
}

int main(int argc, char *argv[]) {
    QCoreApplication a(argc, argv);

    QObject obj;
    auto *helper = new Helper(&obj);
    QObject::connect(&a, &QCoreApplication::aboutToQuit, helper, &Helper::aboutToQuit);
    helper->setProperty("value", "initial value");
    QDBusConnection::systemBus().registerObject("/", &obj);

    QObject objMsiEc;
    auto *helperMsiEc = new MsiEc(&objMsiEc);
    QDBusConnection::systemBus().registerObject("/msi_ec", &objMsiEc);

    if (!QDBusConnection::systemBus().registerService(SERVICE_NAME)) {
        fprintf(stderr, "%s\n", qPrintable(QDBusConnection::systemBus().lastError().message()));
        exit(1);
    }

    return QCoreApplication::exec();
}
