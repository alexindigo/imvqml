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
    void evictionCompletion_data() {
        QTest::addColumn<bool>("all");
        QTest::addColumn<bool>("contended");
        QTest::newRow("clear") << true << false;
        QTest::newRow("invalidate under lock") << false << true;
    }

    void evictionCompletion() {
        QFETCH(bool, all);
        QFETCH(bool, contended);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        ImvCache policy;
        policy.setDirectory(QUrl::fromLocalFile(directory.path()));
        const auto source = QUrl::fromLocalFile(IMV_FIXTURES "/palette.png");
        QFile encoded(source.toLocalFile());
        QVERIFY(encoded.open(QIODevice::ReadOnly));
        const auto digest =
            QCryptographicHash::hash(encoded.readAll(), QCryptographicHash::Sha256).toHex();
        const auto filename = [&](const QByteArray &hash) {
            return QDir(directory.path()).filePath(hash + '-' + QByteArray(64, 'a') + ".imvqoi");
        };
        const auto target = filename(digest);
        const auto unrelated = filename(QByteArray(64, 'b'));
        for (const auto &path : {target, unrelated}) {
            QFile input(path);
            QVERIFY(input.open(QIODevice::WriteOnly));
        }
        QLockFile other(QDir(directory.path()).filePath(".imvqml.lock"));
        if (contended)
            QVERIFY(other.tryLock(0));
        bool targetGoneAtSignal = false;
        bool unrelatedGoneAtSignal = false;
        const auto meta = policy.metaObject();
        const auto signal = meta->indexOfSignal("cleared(int,QVariantMap)");
        QVERIFY2(signal >= 0, "No completion signal: the caller cannot observe actual eviction");
        QSignalSpy done(&policy, meta->method(signal));
        const auto method = meta->method(meta->indexOfMethod(all ? "clear()" : "invalidate(QUrl)"));
        QCOMPARE(method.returnType(), int(QMetaType::Int));
        connect(&policy, &ImvCache::cleared, this, [&] {
            targetGoneAtSignal = !QFileInfo::exists(target);
            unrelatedGoneAtSignal = !QFileInfo::exists(unrelated);
        });
        int ticket = 0;
        QVERIFY(all ? method.invoke(&policy, Qt::DirectConnection, Q_RETURN_ARG(int, ticket))
                    : method.invoke(&policy, Qt::DirectConnection, Q_RETURN_ARG(int, ticket),
                                    Q_ARG(QUrl, source)));
        QVERIFY(ticket > 0);
        QVERIFY(done.isEmpty());
        if (contended) {
            QTest::qWait(300);
            QVERIFY(done.isEmpty());
            QVERIFY(QFileInfo::exists(target));
            other.unlock();
        }
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(done.first().at(0).toInt(), ticket);
        const auto report = done.first().at(1).toMap();
        QCOMPARE(report.value("stage").toString(), "complete");
        QCOMPARE(report.value("removed").toInt(), all ? 2 : 1);
        QCOMPARE(report.value("kept").toInt(), 0);
        QCOMPARE(report.value("failed").toInt(), 0);
        QVERIFY(targetGoneAtSignal);
        QCOMPARE(unrelatedGoneAtSignal, all);
    }

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

        interest.reset();
        asset.reset();
        QSignalSpy eviction(&policy, &ImvCache::cleared);
        QVERIFY(policy.invalidate(source) > 0);
        QTRY_COMPARE(eviction.size(), 1);
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
        interest.reset();
        asset.reset();
        QVERIFY(policy.clear() > 0);
        QTRY_COMPARE(eviction.size(), 2);
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

    void thumbnailRejectsReplacedSource() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        qputenv("XDG_CACHE_HOME", QFile::encodeName(directory.path()));
        ImvCache policy;
        QSignalSpy done(&policy, &ImvCache::thumbnailFinished);
        const auto path = directory.filePath("transient.png");
        QVERIFY(QFile::copy(IMV_FIXTURES "/palette.png", path));
        const int id = policy.exportThumbnail(QUrl::fromLocalFile(path));
        QVERIFY(id > 0);
        QVERIFY(QFile::remove(path));
        QVERIFY(QFile::copy(IMV_FIXTURES "/alpha.png", path));
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(done.first().at(0).toInt(), id);
        const auto result = done.first().at(1).toMap();
        QCOMPARE(result.value("stage").toString(), "store");
        QCOMPARE(result.value("code").toString(), "write-failed");
        QVERIFY(!result.contains("destination"));
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
        a.lease.reset();

        QLockFile other(QDir(directory.path()).filePath(".imvqml.lock"));
        QVERIFY(other.tryLock(0));
        std::optional<ImageStore::Eviction> completion;
        auto done = [&](ImageStore::Eviction result) { completion = std::move(result); };
        if (all)
            store.clear(done);
        else
            store.invalidate(input('a'), done);
        // An observed lookup is a barrier behind the locked invalidation;
        // release the other owner's lock only after that operation has run.
        bool settled = false;
        store.lookup(input('a'), spec, [&](ImageStore::Lookup) { settled = true; });
        QTRY_VERIFY(settled);
        QVERIFY(!completion);
        other.unlock();
        QTRY_VERIFY(completion);
        QCOMPARE(completion->removed, 1);
        QCOMPARE(completion->kept, all ? 1 : 0);
        QCOMPARE(completion->failed, 0);
        QVERIFY(!completion->error);
        QVERIFY(!QFileInfo::exists(a.path));
        QVERIFY(QFileInfo::exists(b.path));

        std::optional<bool> staleA, staleB;
        store.lookup(input('a'), spec, [&](ImageStore::Lookup r) { staleA = bool(r.value); });
        store.lookup(input('b'), spec, [&](ImageStore::Lookup r) { staleB = bool(r.value); });
        QTRY_VERIFY(staleA.has_value() && staleB.has_value());
        QCOMPARE(*staleA, false);
        QCOMPARE(*staleB, true);
        QVERIFY(!QFileInfo::exists(a.path));
        QVERIFY(QFileInfo::exists(b.path));
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
