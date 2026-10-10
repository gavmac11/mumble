// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "PQFileTransfer/engine/FileTransferTransport.h"

#include <QList>
#include <QSemaphore>
#include <QTest>
#include <QThread>

#include <limits>
#include <memory>

struct Delivery {
	QList< unsigned int > targets;
	QByteArray payload;
	QByteArray transferId;
	quint64 index = 0;
	std::optional< quint64 > count;
	int controlCalls    = 0;
	int chunkCalls      = 0;
	bool receiverThread = false;
	QSemaphore completed;
};

// Like ServerHandler, these send methods are not registered with Qt's
// metaobject system. Changing production transport back to method-name
// dispatch must fail this test, rather than silently losing a transfer.
class Receiver : public QObject {
	Q_OBJECT
public:
	explicit Receiver(std::shared_ptr< Delivery > delivery) : m_delivery(std::move(delivery)) {}
	void sendFileTransferControl(const QList< unsigned int > &targets, const QByteArray &payload) {
		m_delivery->targets = targets;
		m_delivery->payload = payload;
		m_delivery->controlCalls++;
		m_delivery->receiverThread = QThread::currentThread() == thread();
		m_delivery->completed.release();
	}
	void sendFileData(const QByteArray &transferId, quint64 index, std::optional< quint64 > count,
					  const QByteArray &data) {
		m_delivery->transferId = transferId;
		m_delivery->index      = index;
		m_delivery->count      = count;
		m_delivery->payload    = data;
		m_delivery->chunkCalls++;
		m_delivery->receiverThread = QThread::currentThread() == thread();
		m_delivery->completed.release();
	}

private:
	std::shared_ptr< Delivery > m_delivery;
};

class TestFileTransferTransport : public QObject {
	Q_OBJECT
private slots:
	void controlIsQueuedAndCopiesPayload() {
		auto delivery = std::make_shared< Delivery >();
		Receiver receiver(delivery);
		QByteArray payload("a\0b\xff", 4);
		const QByteArray expected = payload;
		QVERIFY(PQFT::Transport::queueControl(&receiver, std::numeric_limits< unsigned int >::max(), payload));
		QCOMPARE(delivery->controlCalls, 0);
		payload.fill('x');
		QCoreApplication::sendPostedEvents(&receiver, QEvent::MetaCall);
		QCOMPARE(delivery->controlCalls, 1);
		QCOMPARE(delivery->targets, QList< unsigned int >{ std::numeric_limits< unsigned int >::max() });
		QCOMPARE(delivery->payload, expected);
		QVERIFY(delivery->receiverThread);
	}

	void chunkValues_data() {
		QTest::addColumn< quint64 >("index");
		QTest::addColumn< quint64 >("total");
		QTest::newRow("present-zero") << quint64(0) << quint64(0);
		QTest::newRow("wide-values") << (quint64(1) << 40) << std::numeric_limits< quint64 >::max();
	}

	void chunkValues() {
		QFETCH(quint64, index);
		QFETCH(quint64, total);
		auto delivery = std::make_shared< Delivery >();
		Receiver receiver(delivery);
		QByteArray id(16, '\x01');
		QByteArray data("\0\xff\x01", 3);
		const QByteArray expectedId   = id;
		const QByteArray expectedData = data;
		QVERIFY(PQFT::Transport::queueChunk(&receiver, id, index, total, data));
		QCOMPARE(delivery->chunkCalls, 0);
		id.fill('i');
		data.fill('d');
		QCoreApplication::sendPostedEvents(&receiver, QEvent::MetaCall);
		QCOMPARE(delivery->chunkCalls, 1);
		QCOMPARE(delivery->transferId, expectedId);
		QCOMPARE(delivery->payload, expectedData);
		QCOMPARE(delivery->index, index);
		QVERIFY(delivery->count.has_value());
		QCOMPARE(*delivery->count, total);
	}

	void destroyedReceiverCancelsPendingCalls() {
		auto delivery = std::make_shared< Delivery >();
		auto receiver = std::make_unique< Receiver >(delivery);
		QVERIFY(PQFT::Transport::queueControl(receiver.get(), 42, QByteArray("control")));
		QVERIFY(PQFT::Transport::queueChunk(receiver.get(), QByteArray(16, 'i'), 0, 1, QByteArray("data")));
		receiver.reset();
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		QCOMPARE(delivery->controlCalls, 0);
		QCOMPARE(delivery->chunkCalls, 0);
	}

	void replacementDoesNotReceiveOldConnectionData() {
		auto oldDelivery = std::make_shared< Delivery >();
		auto oldReceiver = std::make_unique< Receiver >(oldDelivery);
		QVERIFY(PQFT::Transport::queueControl(oldReceiver.get(), 42, QByteArray("old")));
		oldReceiver.reset();
		auto newDelivery = std::make_shared< Delivery >();
		Receiver newReceiver(newDelivery);
		QVERIFY(PQFT::Transport::queueControl(&newReceiver, 43, QByteArray("new")));
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		QCOMPARE(oldDelivery->controlCalls, 0);
		QCOMPARE(newDelivery->controlCalls, 1);
		QCOMPARE(newDelivery->payload, QByteArray("new"));
	}

	void executesOnReceiverThread() {
		auto delivery = std::make_shared< Delivery >();
		QThread worker;
		Receiver *receiver = std::make_unique< Receiver >(delivery).release();
		receiver->moveToThread(&worker);
		connect(&worker, &QThread::finished, receiver, &QObject::deleteLater);
		worker.start();
		const bool queued    = PQFT::Transport::queueControl(receiver, 7, QByteArray("thread"));
		const bool delivered = delivery->completed.tryAcquire(1, 5000);
		worker.quit();
		worker.wait();
		QVERIFY(queued);
		QVERIFY(delivered);
		QVERIFY(delivery->receiverThread);
		QCOMPARE(delivery->targets, QList< unsigned int >{ 7 });
	}

	void connectionSnapshotDoesNotFollowReplacement() {
		auto oldDelivery                                = std::make_shared< Delivery >();
		auto current                                    = std::make_shared< Receiver >(oldDelivery);
		const std::weak_ptr< Receiver > snapshot        = current;
		const std::shared_ptr< Receiver > oldConnection = current;
		auto newDelivery                                = std::make_shared< Delivery >();
		current                                         = std::make_shared< Receiver >(newDelivery);
		QVERIFY(PQFT::Transport::queueControl(snapshot, 42, QByteArray("old")));
		QVERIFY(PQFT::Transport::queueChunk(snapshot, QByteArray(16, 'i'), 0, 1, QByteArray("data")));
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		QCOMPARE(oldDelivery->controlCalls, 1);
		QCOMPARE(oldDelivery->chunkCalls, 1);
		QCOMPARE(newDelivery->controlCalls, 0);
		QCOMPARE(newDelivery->chunkCalls, 0);
	}

	void queuedCallsDoNotRetainConnection() {
		auto delivery                            = std::make_shared< Delivery >();
		auto connection                          = std::make_shared< Receiver >(delivery);
		const std::weak_ptr< Receiver > snapshot = connection;
		QVERIFY(PQFT::Transport::queueControl(snapshot, 42, QByteArray("old")));
		QVERIFY(PQFT::Transport::queueChunk(snapshot, QByteArray(16, 'i'), 0, 1, QByteArray("data")));
		connection.reset();
		QVERIFY(snapshot.expired());
		QVERIFY(!PQFT::Transport::queueControl(snapshot, 42, QByteArray("expired")));
		QVERIFY(!PQFT::Transport::queueChunk(snapshot, QByteArray(16, 'i'), 0, 1, QByteArray("expired")));
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		QCOMPARE(delivery->controlCalls, 0);
		QCOMPARE(delivery->chunkCalls, 0);
	}

	void disconnectedTransportRejectsQueue() {
		QVERIFY(!PQFT::Transport::queueControl< Receiver >(nullptr, 7, QByteArray("control")));
		QVERIFY(!PQFT::Transport::queueChunk< Receiver >(nullptr, QByteArray(16, 'i'), 0, 1, QByteArray("data")));
	}
};

QTEST_GUILESS_MAIN(TestFileTransferTransport)
#include "TestFileTransferTransport.moc"
