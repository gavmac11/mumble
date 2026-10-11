// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "Connection.h"

#include <QtNetwork/QTcpServer>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include <limits>

// A real connected socket with controllable plaintext backlog. Avoid timing
// assumptions about kernel send buffers in the boundary/lifetime checks.
class QueuedSocket : public QSslSocket {
public:
	qint64 pending = 0;
	int writes     = 0;
	qint64 bytesToWrite() const override { return pending; }

protected:
	qint64 writeData(const char *, qint64 size) override {
		++writes;
		pending += size;
		return size;
	}
};

class TestConnectionOutputQueue : public QObject {
	Q_OBJECT
private slots:
	void admitsExactBoundary();
	void admitsLargestProtocolFrame();
	void disconnectedSocketDoesNotWrite();
	void refusesFurtherWritesBeforeDeferredClose();
	void unboundedDefault();
	void emptyWriteDoesNotDisconnect();
	void largeQueueDoesNotOverflow();
	void destroyingConnectionCancelsDeferredClose();

private:
	static QueuedSocket *connectedSocket(QTcpServer &server) {
		if (!server.listen(QHostAddress::LocalHost))
			return nullptr;
		auto *socket = new QueuedSocket;
		socket->connectToHost(QHostAddress::LocalHost, server.serverPort());
		if (!socket->waitForConnected(1000)) {
			delete socket;
			return nullptr;
		}
		return socket;
	}
};

void TestConnectionOutputQueue::admitsExactBoundary() {
	QTcpServer server;
	auto *socket = connectedSocket(server);
	QVERIFY(socket);
	Connection connection(nullptr, socket);
	connection.setMaxPendingSendBytes(10);
	socket->pending = 7;
	connection.sendMessage(QByteArray(3, 'x'));
	QCOMPARE(socket->writes, 1);
	QCOMPARE(socket->pending, 10);
	QCOMPARE(socket->state(), QAbstractSocket::ConnectedState);
}

void TestConnectionOutputQueue::admitsLargestProtocolFrame() {
	QTcpServer server;
	auto *socket = connectedSocket(server);
	QVERIFY(socket);
	Connection connection(nullptr, socket);
	connection.setMaxPendingSendBytes(16 * 1024 * 1024);
	// Existing messageToNetwork permits this payload plus the six-byte header.
	connection.sendMessage(QByteArray(0x7fffff + 6, 'x'));
	QCOMPARE(socket->writes, 1);
	QCOMPARE(socket->state(), QAbstractSocket::ConnectedState);
}

void TestConnectionOutputQueue::disconnectedSocketDoesNotWrite() {
	auto *socket = new QueuedSocket;
	Connection connection(nullptr, socket);
	connection.setMaxPendingSendBytes(1);
	connection.sendMessage(QByteArray(2, 'x'));
	QCOMPARE(socket->writes, 0);
	QCOMPARE(socket->state(), QAbstractSocket::UnconnectedState);
}

void TestConnectionOutputQueue::refusesFurtherWritesBeforeDeferredClose() {
	QTcpServer server;
	auto *socket = connectedSocket(server);
	QVERIFY(socket);
	Connection connection(nullptr, socket);
	QSignalSpy closed(&connection, &Connection::connectionClosed);
	connection.setMaxPendingSendBytes(10);
	socket->pending = 8;
	connection.sendMessage(QByteArray(3, 'x'));
	// Even if the backlog drains or policy changes before the callback, do
	// not enqueue more frames after deciding to close this connection.
	socket->pending = 0;
	connection.setMaxPendingSendBytes(0);
	connection.sendMessage(QByteArray(1, 'x'));
	QCOMPARE(socket->writes, 0);
	QCOMPARE(closed.count(), 0);
	QCOMPARE(socket->state(), QAbstractSocket::ConnectedState);
	QTest::ignoreMessage(QtWarningMsg, "Outgoing TCP queue exceeded connection limit; disconnecting slow peer");
	QTRY_COMPARE(closed.count(), 1);
	QCOMPARE(socket->state(), QAbstractSocket::UnconnectedState);
}

void TestConnectionOutputQueue::unboundedDefault() {
	QTcpServer server;
	auto *socket = connectedSocket(server);
	QVERIFY(socket);
	Connection connection(nullptr, socket);
	socket->pending = 64 * 1024 * 1024;
	connection.sendMessage(QByteArray(1, 'x'));
	QCOMPARE(socket->writes, 1);
}

void TestConnectionOutputQueue::emptyWriteDoesNotDisconnect() {
	QTcpServer server;
	auto *socket = connectedSocket(server);
	QVERIFY(socket);
	Connection connection(nullptr, socket);
	connection.setMaxPendingSendBytes(1);
	socket->pending = 2;
	connection.sendMessage(QByteArray());
	QCOMPARE(socket->writes, 0);
	QCoreApplication::processEvents();
	QCOMPARE(socket->state(), QAbstractSocket::ConnectedState);
}

void TestConnectionOutputQueue::largeQueueDoesNotOverflow() {
	QTcpServer server;
	auto *socket = connectedSocket(server);
	QVERIFY(socket);
	Connection connection(nullptr, socket);
	connection.setMaxPendingSendBytes(std::numeric_limits< qint64 >::max());
	socket->pending = std::numeric_limits< qint64 >::max();
	connection.sendMessage(QByteArray(1, 'x'));
	QCOMPARE(socket->writes, 0);
	QTest::ignoreMessage(QtWarningMsg, "Outgoing TCP queue exceeded connection limit; disconnecting slow peer");
	QTRY_COMPARE(socket->state(), QAbstractSocket::UnconnectedState);
}

void TestConnectionOutputQueue::destroyingConnectionCancelsDeferredClose() {
	QTcpServer server;
	auto *socket = connectedSocket(server);
	QVERIFY(socket);
	auto *connection = new Connection(nullptr, socket);
	connection->setMaxPendingSendBytes(1);
	connection->sendMessage(QByteArray(2, 'x'));
	QCOMPARE(socket->writes, 0);
	delete connection;
	QCoreApplication::processEvents();
}

QTEST_GUILESS_MAIN(TestConnectionOutputQueue)
#include "TestConnectionOutputQueue.moc"
