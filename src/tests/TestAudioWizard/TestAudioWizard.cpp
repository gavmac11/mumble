// Copyright The Mumble Developers. All rights reserved.
// BSD-style license: see LICENSE at the source root.
#include "AudioWizard.h"
#include "Global.h"

#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

class SilentOutput : public AudioOutput {
	void run() override {}
public:
	~SilentOutput() override { wait(); }
};

class FixtureInput : public AudioInputRegistrar {
public:
	FixtureInput() : AudioInputRegistrar("Private fixture input") {}
	AudioInput *create() override { return nullptr; }
	const QVariant getDeviceChoice() override { return 0; }
	const QList< audioDevice > getDeviceChoices() override { return { { "Private input", 0 } }; }
	void setDeviceChoice(const QVariant &, Settings &) override {}
	bool canEcho(EchoCancelOptionID, const QString &) const override { return false; }
	bool isMicrophoneAccessDeniedByOS() override { return true; }
};
class FixtureOutput : public AudioOutputRegistrar {
public:
	bool available = false;
	FixtureOutput() : AudioOutputRegistrar("Private fixture output") {}
	AudioOutput *create() override { return available ? new SilentOutput() : nullptr; }
	const QVariant getDeviceChoice() override { return 0; }
	const QList< audioDevice > getDeviceChoices() override { return { { "Private output", 0 } }; }
	void setDeviceChoice(const QVariant &, Settings &) override {}
};

class TestAudioWizard : public QObject {
	Q_OBJECT
private slots:
	void unavailableDevices_data() {
		QTest::addColumn< bool >("outputAvailable");
		QTest::newRow("pending-or-denied-input") << true;
		QTest::newRow("both-unavailable") << false;
	}
	void unavailableDevices() {
		QFETCH(bool, outputAvailable);
		QTemporaryDir root;
		QVERIFY(root.isValid());
		Global global(root.filePath("private-settings.json"));
		global.s.qlShortcuts.clear();
		auto *previousGlobal         = Global::g_global_struct;
		auto *previousInputs         = AudioInputRegistrar::qmNew;
		auto *previousOutputs        = AudioOutputRegistrar::qmNew;
		const QString previousInput  = AudioInputRegistrar::current;
		const QString previousOutput = AudioOutputRegistrar::current;
		QMap< QString, AudioInputRegistrar * > inputs;
		QMap< QString, AudioOutputRegistrar * > outputs;
		Global::g_global_struct     = &global;
		AudioInputRegistrar::qmNew  = &inputs;
		AudioOutputRegistrar::qmNew = &outputs;
		const auto restore          = qScopeGuard([&]() {
			Audio::stop();
			AudioInputRegistrar::qmNew    = previousInputs;
			AudioOutputRegistrar::qmNew   = previousOutputs;
			AudioInputRegistrar::current  = previousInput;
			AudioOutputRegistrar::current = previousOutput;
			Global::g_global_struct       = previousGlobal;
		});
		FixtureInput input;
		FixtureOutput output;
		output.available              = outputAvailable;
		AudioInputRegistrar::current  = input.name;
		AudioOutputRegistrar::current = output.name;
		AudioWizard wizard(nullptr);
		wizard.restart();
		wizard.next();
		QCOMPARE(wizard.currentPage(), wizard.qwpDevice);
		wizard.on_qcbInputDevice_activated(0);
		wizard.on_qcbOutputDevice_activated(0);
		wizard.on_Ticker_timeout();
		QVERIFY(!wizard.qwpDevice->isComplete());
		QVERIFY(!wizard.validateCurrentPage());
		QVERIFY(wizard.qlDeviceStatus->text().contains("microphone access"));
		if (!outputAvailable)
			QVERIFY(wizard.qlDeviceStatus->text().contains("Audio output is unavailable"));
		// Recovery of output alone must not let an unavailable input pass.
		output.available = true;
		Audio::startOutput(output.name);
		wizard.on_Ticker_timeout();
		QVERIFY(!wizard.qwpDevice->isComplete());
		QVERIFY(wizard.qlDeviceStatus->text().contains("microphone access"));
		QVERIFY(!wizard.qlDeviceStatus->text().contains("Audio output is unavailable"));
		inputs.remove(input.name);
		wizard.on_qcbInput_activated(0);
		wizard.on_qcbInputDevice_activated(0);
		QVERIFY(!wizard.qwpDevice->isComplete());
		outputs.remove(output.name);
		wizard.on_qcbOutput_activated(0);
		wizard.on_qcbOutputDevice_activated(0);
		QVERIFY(!wizard.validateCurrentPage());
		wizard.reject();
	}
};

QTEST_MAIN(TestAudioWizard)
#include "TestAudioWizard.moc"
