// SPDX-License-Identifier: LGPL-3.0-only
#include "image-pipeline.h"
#include <imvqml/imvcache.h>

#include <QCryptographicHash>
#include <QFile>
#include <QLockFile>
#include <QtTest>
#include <sys/stat.h>

using namespace Imv;

class StoreTest : public QObject {
    Q_OBJECT
private slots:
    void pixelsInvalidationAndThumbnails() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        ImvCache policy;
        const auto source = QUrl::fromLocalFile(IMV_FIXTURES "/palette.png");
        policy.setDirectory(QUrl::fromLocalFile(directory.path()));
        policy.setEnabled(true);
        Pool pool;
        ImagePipeline pipeline(pool);
        Asset asset;
        Failure failure;
        const DecodeSpec spec{DecodeSpec::Pixels, {32, 32}};
        const auto request = [&] {
            asset.reset();
            return pipeline.request(openSource(source), spec, [&](Asset value, Failure error) {
                asset = std::move(value);
                failure = error;
            });
        };
        const auto files = [&] {
            return QDir(directory.path()).entryList({"*.imvqoi"}, QDir::Files);
        };
        auto interest = request();
        QTRY_VERIFY(asset || failure);
        QVERIFY(asset);
        QCOMPARE(asset->pixels.pixelColor(8, 8), QColor(Qt::red));
        QTRY_COMPARE(files().size(), 1);
        QCOMPARE(pipeline.submissions(), 1u);
        interest = request();
        QTRY_VERIFY(asset);
        QCOMPARE(asset->pixels.pixelColor(24, 8), QColor(Qt::green));
        QCOMPARE(pipeline.submissions(), 1u);

        ImageStore nextVersion("new-decode-version");
        nextVersion.configure(directory.path(), ImageStore::DefaultBudget, true);
        bool checked = false;
        nextVersion.lookup(openSource(source).fd, spec, [&](ImageStore::Lookup result) {
            checked = !result.path.isEmpty() && !result.value;
        });
        QTRY_VERIFY(checked);

        policy.invalidate(source);
        interest = request();
        QTRY_VERIFY(asset);
        QCOMPARE(pipeline.submissions(), 2u);
        QTRY_COMPARE(files().size(), 1);
        QFile corrupted(QDir(directory.path()).filePath(files().first()));
        QVERIFY(corrupted.open(QIODevice::WriteOnly | QIODevice::Truncate));
        corrupted.write("not a cache entry");
        corrupted.close();
        interest = request();
        QTRY_VERIFY(asset);
        QCOMPARE(pipeline.submissions(), 3u);
        policy.clear();
        QTRY_VERIFY(files().isEmpty());
        policy.setEnabled(false);

        qputenv("XDG_CACHE_HOME", QFile::encodeName(directory.path()));
        QSignalSpy done(&policy, &ImvCache::thumbnailFinished);
        const int id = policy.exportThumbnail(source, 128);
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(done.first().at(0).toInt(), id);
        const auto result = done.first().at(1).toMap();
        QCOMPARE(result.value("stage").toString(), "complete");
        const auto uri = source.toEncoded();
        const auto name = QCryptographicHash::hash(uri, QCryptographicHash::Md5).toHex();
        const auto path = result.value("destination").toUrl().toLocalFile();
        QCOMPARE(path, directory.path() + "/thumbnails/normal/" + name + ".png");
        const QImage image(path);
        QCOMPARE(image.size(), QSize(64, 32));
        QCOMPARE(image.pixelColor(16, 16), QColor(Qt::red));
        QCOMPARE(image.pixelColor(48, 16), QColor(Qt::green));
        QCOMPARE(image.text("Thumb::URI"), QString::fromUtf8(uri));
        QCOMPARE(
            image.text("Thumb::MTime"),
            QString::number(QFileInfo(source.toLocalFile()).lastModified().toSecsSinceEpoch()));
        struct stat bits{};
        QVERIFY(::stat(QFile::encodeName(path).constData(), &bits) == 0);
        QCOMPARE(bits.st_mode & 0777u, 0600u);
    }

    void thumbnailBurst() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        qputenv("XDG_CACHE_HOME", QFile::encodeName(directory.path()));
        ImvCache policy;
        QSignalSpy done(&policy, &ImvCache::thumbnailFinished);
        QSet<int> ids;
        for (int i = 0; i < 50; ++i) {
            const auto path = directory.filePath(QString::number(i) + ".png");
            QVERIFY(QFile::copy(IMV_FIXTURES "/palette.png", path));
            ids.insert(policy.exportThumbnail(QUrl::fromLocalFile(path), 128));
        }
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 50, 15000);
        for (const auto &completion : done) {
            QVERIFY(ids.remove(completion[0].toInt()));
            const auto result = completion[1].toMap();
            QVERIFY2(result.value("stage") == "complete",
                     qPrintable(result.value("code").toString()));
            const auto path = result.value("destination").toUrl().toLocalFile();
            QCOMPARE(QImage(path).pixelColor(16, 16), QColor(Qt::red));
        }
        QVERIFY(ids.isEmpty());
    }

    void contendedInvalidation_data() {
        QTest::addColumn<bool>("all");
        QTest::newRow("single source") << false;
        QTest::newRow("clear all") << true;
    }

    void contendedInvalidation() {
        QFETCH(bool, all);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        ImageStore store("version");
        const DecodeSpec spec{DecodeSpec::Pixels, {8, 8}};
        QImage pixels(8, 8, QImage::Format_RGBA8888);
        pixels.fill(Qt::red);
        const Decoded value{{"png", {8, 8}, 1, 1}, pixels, {}};
        const auto input = [](char byte) { return openSource(QByteArray(1, byte)).fd; };
        ImageStore::Lookup a, b;
        store.configure(directory.path(), 4096, true);
        store.lookup(input('a'), spec, [&](ImageStore::Lookup r) { a = std::move(r); });
        store.lookup(input('b'), spec, [&](ImageStore::Lookup r) { b = std::move(r); });
        QTRY_VERIFY(a.lease && b.lease);
        store.publish(a, value);
        store.publish(b, value);
        QTRY_VERIFY(QFileInfo::exists(a.path) && QFileInfo::exists(b.path));

        QLockFile other(QDir(directory.path()).filePath(".imvqml.lock"));
        QVERIFY(other.tryLock(0));
        if (all)
            store.clear();
        else
            store.invalidate(input('a'));
        // An observed lookup is a barrier behind the locked invalidation;
        // release the other owner's lock only after that operation has run.
        bool settled = false;
        store.lookup(input('a'), spec, [&](ImageStore::Lookup) { settled = true; });
        QTRY_VERIFY(settled);
        other.unlock();

        std::optional<bool> staleA, staleB;
        store.lookup(input('a'), spec, [&](ImageStore::Lookup r) { staleA = bool(r.value); });
        store.lookup(input('b'), spec, [&](ImageStore::Lookup r) { staleB = bool(r.value); });
        QTRY_VERIFY(staleA.has_value() && staleB.has_value());
        QCOMPARE(*staleA, false);
        QCOMPARE(*staleB, !all);
        QVERIFY(!QFileInfo::exists(a.path));
        QCOMPARE(QFileInfo::exists(b.path), !all);
    }

    void evictionHonorsLease() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        ImageStore store("version");
        const DecodeSpec spec{DecodeSpec::Pixels, {32, 32}};
        QImage pixels(32, 16, QImage::Format_RGBA8888);
        pixels.fill(Qt::red);
        Decoded value{{"png", {64, 32}, 1, 1}, pixels, {}};
        const auto input = [](char bytes) { return openSource(QByteArray(1, bytes)).fd; };
        const auto files = [&] {
            return QDir(directory.path()).entryInfoList({"*.imvqoi"}, QDir::Files);
        };
        store.configure(directory.path(), 250, true);
        ImageStore::Lookup first, second;
        store.lookup(input('a'), spec,
                     [&](ImageStore::Lookup result) { first = std::move(result); });
        QTRY_VERIFY(first.lease);
        store.publish(first, value);
        QTRY_COMPARE(files().size(), 1);
        // Make the budget exactly one encoded entry rather than assuming its
        // compressed length. Both writes encode the same image.
        store.configure(directory.path(), files().first().size(), true);
        store.lookup(input('b'), spec,
                     [&](ImageStore::Lookup result) { second = std::move(result); });
        QTRY_VERIFY(second.lease);
        store.publish(second, value);
        bool checked = false;
        store.lookup(input('b'), spec, [&](ImageStore::Lookup result) { checked = !result.value; });
        QTRY_VERIFY(checked);
        QVERIFY(QFileInfo::exists(first.path));
        const auto previous = first.path;
        first = {};
        store.publish(second, value);
        QTRY_VERIFY(QFileInfo::exists(second.path));
        QVERIFY(!QFileInfo::exists(previous));
    }
};

QTEST_GUILESS_MAIN(StoreTest)
#include "store-test.moc"
