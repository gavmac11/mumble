// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "ChatVideoMixerSink.h"

#include "AudioOutput.h"
#include "AudioOutputBuffer.h"
#include "AudioOutputToken.h"
#include "ChatVideoPlayer.h"

#include <QtCore/QMutex>
#include <QtCore/QMutexLocker>

#include <atomic>
#include <cstring>
#include <vector>

// Global.h defines the global macro g and therefore has to be the final include.
#include "Global.h"

/// State shared between the sink (GUI thread) and its mixer buffer (audio
/// thread). The buffer is owned and deleted by AudioOutput, so neither side
/// may point at the other directly.
struct ChatVideoMixerShared {
	/// Guards source: the GUI thread clears it before the source is deleted
	QMutex mutex;
	ChatVideoAudioSource *source = nullptr;

	std::atomic< bool > stopped{ false };
	std::atomic< bool > suspended{ false };
	/// Set once AudioOutput deleted the buffer (stopped, wiped or restarted)
	std::atomic< bool > detached{ false };
	/// Whether the last mixer pull found less audio than it asked for
	std::atomic< bool > starved{ false };
	/// Stereo frames handed to the mixer
	std::atomic< qint64 > framesPlayed{ 0 };
};

namespace {
constexpr unsigned int MIXER_CHANNELS  = 2;
constexpr qint64 MIXER_BYTES_PER_FRAME = MIXER_CHANNELS * static_cast< qint64 >(sizeof(qint16));
constexpr float MIXER_S16_TO_FLOAT     = 1.0f / 32768.0f;
/// Frames preallocated so the audio thread normally never allocates
constexpr unsigned int MIXER_PREALLOC_FRAMES = 4800;

class ChatVideoMixerBuffer : public AudioOutputBuffer {
public:
	explicit ChatVideoMixerBuffer(std::shared_ptr< ChatVideoMixerShared > shared) : m_shared(std::move(shared)) {
		bStereo     = true;
		iBufferSize = 0;
		resizeBuffer(MIXER_PREALLOC_FRAMES * MIXER_CHANNELS);
		m_pcm.resize(MIXER_PREALLOC_FRAMES * MIXER_CHANNELS);
	}

	~ChatVideoMixerBuffer() override { m_shared->detached = true; }

	bool prepareSampleBuffer(unsigned int frameCount) override {
		if (m_shared->stopped)
			return false;

		const unsigned int sampleCount = frameCount * MIXER_CHANNELS;
		resizeBuffer(sampleCount);
		if (m_pcm.size() < sampleCount)
			m_pcm.resize(sampleCount);

		qint64 frames = 0;
		if (!m_shared->suspended) {
			QMutexLocker lock(&m_shared->mutex);
			if (m_shared->source) {
				frames = m_shared->source->take(reinterpret_cast< char * >(m_pcm.data()),
												static_cast< qint64 >(frameCount) * MIXER_BYTES_PER_FRAME)
						 / MIXER_BYTES_PER_FRAME;
			}
		}

		const unsigned int samples = static_cast< unsigned int >(frames) * MIXER_CHANNELS;
		for (unsigned int i = 0; i < samples; ++i)
			pfBuffer[i] = static_cast< float >(m_pcm[i]) * MIXER_S16_TO_FLOAT;
		if (samples < sampleCount)
			std::memset(pfBuffer + samples, 0, sizeof(float) * (sampleCount - samples));

		if (!m_shared->suspended) {
			m_shared->framesPlayed += frames;
			m_shared->starved = frames < static_cast< qint64 >(frameCount);
		}
		// Stay registered while paused or starved; the player stops the sink when done.
		return true;
	}

private:
	std::shared_ptr< ChatVideoMixerShared > m_shared;
	std::vector< qint16 > m_pcm;
};
} // namespace

ChatVideoMixerSink *ChatVideoMixerSink::create(QObject *parent) {
	// Unit tests may drive the player without any global client state.
	if (!Global::g_global_struct)
		return nullptr;

	AudioOutputPtr ao = Global::get().ao;
	if (!ao || !ao->isAlive())
		return nullptr;

	const unsigned int rate = ao->getMixerFreq();
	if (rate == 0)
		return nullptr;

	auto shared = std::make_shared< ChatVideoMixerShared >();
	if (!ao->addSampleBuffer(new ChatVideoMixerBuffer(shared)))
		return nullptr;

	return new ChatVideoMixerSink(std::move(shared), static_cast< int >(rate), parent);
}

ChatVideoMixerSink::ChatVideoMixerSink(std::shared_ptr< ChatVideoMixerShared > shared, int sampleRate,
									   QObject *parent)
	: QObject(parent), m_shared(std::move(shared)), m_sampleRate(sampleRate) {
}

ChatVideoMixerSink::~ChatVideoMixerSink() {
	stop();
}

void ChatVideoMixerSink::start(ChatVideoAudioSource *source) {
	QMutexLocker lock(&m_shared->mutex);
	m_shared->source = source;
}

void ChatVideoMixerSink::stop() {
	{
		QMutexLocker lock(&m_shared->mutex);
		m_shared->source = nullptr;
	}
	m_shared->stopped = true;
}

void ChatVideoMixerSink::suspend() {
	m_shared->suspended = true;
}

void ChatVideoMixerSink::resume() {
	m_shared->suspended = false;
}

ChatVideoMixerSink::State ChatVideoMixerSink::state() const {
	if (m_shared->stopped || m_shared->detached)
		return StoppedState;
	if (m_shared->suspended)
		return SuspendedState;
	// Only this (GUI) thread changes source, so reading it here needs no lock.
	if (m_shared->starved && (!m_shared->source || m_shared->source->bufferedBytes() == 0))
		return IdleState;
	return ActiveState;
}

ChatVideoMixerSink::Error ChatVideoMixerSink::error() const {
	return (m_shared->detached && !m_shared->stopped) ? FatalError : NoError;
}

qint64 ChatVideoMixerSink::processedUSecs() const {
	return m_shared->framesPlayed * 1000000 / m_sampleRate;
}
