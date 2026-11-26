// SPDX-License-Identifier: LGPL-3.0-only
#include <imvqml/imvimagejob.h>

#include <QCoreApplication>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    Imv::ImvImageJob job;
    return job.metaObject()->indexOfSignal("finished(int,QVariantMap)") >= 0 ? 0 : 1;
}
