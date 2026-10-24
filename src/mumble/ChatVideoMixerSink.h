// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_CHATVIDEOMIXERSINK_H_
#define MUMBLE_MUMBLE_CHATVIDEOMIXERSINK_H_

#include <QtCore/QObject>
#include <QtCore/QtGlobal>

#include <memory>

class ChatVideoAudioSource;
struct ChatVideoMixerShared;

/// Plays chat video audio through Mumble's own audio output (the mixer that
/// also plays notification sounds), for builds without Qt Multimedia.
///
/// It mirrors the small part of the QAudioSink API that ChatVideoPlayer uses,
/// so the player's clock logic works unchanged: processedUSecs() counts the
/// audio the mixer actually consumed, and state() reports IdleState while the
/// source runs dry. Unlike a pull-mode QAudioSink it keeps pulling after an
/// underrun.
///
/// All methods are called on the GUI thread; the mixer pulls on the audio thread.
class ChatVideoMixerSink : public QObject {
public:
	enum State { ActiveState, SuspendedState, StoppedState, IdleState };
	enum Error { NoError, OpenError, IOError, UnderrunError, FatalError };

	/// Creates a sink attached to the running audio output.
	/// @return The sink, or nullptr if no audio output is running
	static ChatVideoMixerSink *create(QObject *parent);
	~ChatVideoMixerSink() override;

	/// Sample rate the source PCM (S16LE stereo) has to be provided in
	int sampleRate() const { return m_sampleRate; }

	void start(ChatVideoAudioSource *source);
	/// Detaches from the source and the mixer for good
	void stop();
	void suspend();
	void resume();

	State state() const;
	/// FatalError once the audio output dropped this stream (e.g. it was restarted)
	Error error() const;
	qint64 processedUSecs() const;
	qint64 bufferSize() const { return 0; }

private:
	ChatVideoMixerSink(std::shared_ptr< ChatVideoMixerShared > shared, int sampleRate, QObject *parent);

	std::shared_ptr< ChatVideoMixerShared > m_shared;
	const int m_sampleRate;
};

#endif // MUMBLE_MUMBLE_CHATVIDEOMIXERSINK_H_
