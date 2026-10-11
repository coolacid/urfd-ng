#pragma once

// Single-band AGC with noise gate and peak limiter for PCM audio
// (160 samples / 20ms frames). Applied between decode and encode
// in the transcoder pipeline.
//
// Design improvements over simple RMS-tracking AGC:
// 1. Sliding RMS window (3 frames / 60ms) — smooths syllable-level variation
// 2. Gate with gain decay — during silence, gain drifts toward the speaker's
//    long-term average gain, so next speech onset starts at the right level
// 3. Fast release after gate — first 5 frames after gate opening use 5x release
//    speed (capped at alpha=0.5) for quicker recovery from pauses
// 4. Asymmetric gain limits — amplification limited more than attenuation
// 5. Peak limiter acts at once on the applied gain, not only on its target
//
// Thread safe: the decode paths of all codecs call it from their own threads.

#include <cstdint>
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <mutex>
#include <chrono>

struct AGCState
{
	float gain = 1.0f;
	uint16_t streamId = 0;

	// Sliding RMS window (3 frames)
	float rmsHistory[3] = {0.0f, 0.0f, 0.0f};
	int rmsIdx = 0;
	int rmsCount = 0;  // frames seen (ramps up to 3)

	// Long-term average gain for this stream (exponential moving average)
	// Used as decay target during gate — maintains speaker-appropriate gain
	float avgGain = 1.0f;

	// Gate tracking
	int gateFrames = 0;    // consecutive frames in gate
	bool wasGated = false; // previous frame was gated
	int postGateFrames = 0; // frames since gate opened (for fast release)

	std::chrono::steady_clock::time_point lastUsed{}; // to drop ended streams
};

class CAGC
{
public:
	void Configure(bool enable, float targetDbfs, float attackMs, float releaseMs,
	               float maxGainUpDb, float maxGainDownDb, float noiseGateDbfs = -55.0f)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_enabled = enable;
		m_targetLinear = powf(10.0f, targetDbfs / 20.0f) * 32767.0f;
		m_maxGain = powf(10.0f, maxGainUpDb / 20.0f);
		m_minGain = powf(10.0f, -maxGainDownDb / 20.0f);
		m_attackAlpha  = (attackMs  > 0.0f) ? 1.0f - expf(-20.0f / attackMs)  : 1.0f;
		m_releaseAlpha = (releaseMs > 0.0f) ? 1.0f - expf(-20.0f / releaseMs) : 1.0f;
		m_noiseGateLinear = powf(10.0f, noiseGateDbfs / 20.0f) * 32767.0f;
	}

	void Configure(bool enable, float targetDbfs, float attackMs, float releaseMs, float maxGainDb)
	{
		Configure(enable, targetDbfs, attackMs, releaseMs, maxGainDb, maxGainDb);
	}

private:
	// Core AGC processing, called with m_mutex held
	void ProcessCore(int16_t *samples, int count, AGCState &state, float &gain_db_out, bool &gate_out)
	{
		// Measure current frame
		float sumSq = 0.0f, peak = 0.0f;
		for (int i = 0; i < count; i++)
		{
			float s = (float)samples[i];
			sumSq += s * s;
			float a = fabsf(s);
			if (a > peak) peak = a;
		}
		float frameRms = sqrtf(sumSq / count);

		// Update sliding RMS window (3 frames = 60ms)
		state.rmsHistory[state.rmsIdx] = frameRms;
		state.rmsIdx = (state.rmsIdx + 1) % 3;
		if (state.rmsCount < 3) state.rmsCount++;

		// Compute windowed RMS (average of available frames)
		float rmsSum = 0.0f;
		for (int i = 0; i < state.rmsCount; i++)
			rmsSum += state.rmsHistory[i];
		float rms = rmsSum / state.rmsCount;

		// Noise gate check
		if (rms < m_noiseGateLinear)
		{
			state.gateFrames++;
			state.wasGated = true;
			state.postGateFrames = 0;

			// Gate with gain decay: slowly drift toward the long-term average gain
			// for this stream. This keeps the gain at the right level for this speaker
			// instead of drifting to unity (which would be wrong for quiet speakers).
			if (state.gateFrames > 5)  // start decay after ~100ms of gate
			{
				float decayAlpha = 0.02f;
				state.gain += decayAlpha * (state.avgGain - state.gain);
			}

			gate_out = true;
			gain_db_out = 20.0f * log10f(std::max(state.gain, 0.001f));
			return;
		}

		// Speech detected — compute desired gain from windowed RMS
		float desiredGain = std::clamp(m_targetLinear / rms, m_minGain, m_maxGain);

		// Peak limiter: never clip
		float peakLimit = 32000.0f / std::max(peak, 1.0f);
		if (desiredGain > peakLimit)
			desiredGain = peakLimit;

		// Determine smoothing alpha
		float alpha;
		if (desiredGain < state.gain)
		{
			// Attack (gain decreasing = loud input)
			alpha = m_attackAlpha;
		}
		else
		{
			// Release (gain increasing = quiet input)
			alpha = m_releaseAlpha;

			// Fast release after gate: 5x speed for first 5 frames (~100ms)
			// This prevents the first syllable after a pause from being too quiet
			if (state.wasGated && state.postGateFrames < 5)
				alpha = std::min(alpha * 5.0f, 0.5f);  // cap at 0.5 to prevent overshoot
		}

		state.gain += alpha * (desiredGain - state.gain);

		// The smoothed gain may still be above the limit after quiet passages
		// (attack only closes part of the gap per frame); cut it at once
		if (state.gain > peakLimit)
			state.gain = peakLimit;

		// Update long-term average gain (slow EMA, ~2s time constant)
		// Only during speech — gives a stable "typical gain" for this speaker
		float avgAlpha = 0.01f;  // ~2s at 20ms frames
		state.avgGain += avgAlpha * (state.gain - state.avgGain);

		// Track post-gate state
		if (state.wasGated)
		{
			state.postGateFrames++;
			if (state.postGateFrames >= 5)
				state.wasGated = false;  // back to normal tracking
		}
		state.gateFrames = 0;

		// Apply gain with hard clip protection
		for (int i = 0; i < count; i++)
		{
			float s = samples[i] * state.gain;
			samples[i] = (int16_t)std::clamp(s, -32767.0f, 32767.0f);
		}

		gain_db_out = 20.0f * log10f(std::max(state.gain, 0.001f));
		gate_out = false;
	}

public:
	// Process without monitoring output
	void Process(int16_t *samples, int count, uint16_t streamId)
	{
		float gain_db;
		bool gate;
		Process(samples, count, streamId, gain_db, gate);
	}

	// Process with monitoring output
	void Process(int16_t *samples, int count, uint16_t streamId, float &gain_db_out, bool &gate_out)
	{
		gate_out = false;
		gain_db_out = 0.0f;
		std::lock_guard<std::mutex> lock(m_mutex);
		if (!m_enabled) return;

		const auto now = std::chrono::steady_clock::now();
		auto it = m_streams.find(streamId);
		if (it == m_streams.end())
		{
			DropEndedStreams(now);
			it = m_streams.emplace(streamId, AGCState{}).first;
			it->second.streamId = streamId;
		}
		it->second.lastUsed = now;

		ProcessCore(samples, count, it->second, gain_db_out, gate_out);
	}

	bool IsEnabled() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_enabled;
	}

private:
	// streams not heard for 30 s have ended
	void DropEndedStreams(std::chrono::steady_clock::time_point now)
	{
		for (auto it = m_streams.begin(); it != m_streams.end(); )
		{
			if (now - it->second.lastUsed > std::chrono::seconds(30))
				it = m_streams.erase(it);
			else
				++it;
		}
	}

	bool  m_enabled = false;
	float m_targetLinear = 3277.0f;    // -20 dBFS default
	float m_maxGain = 10.0f;          // +20 dB up
	float m_minGain = 0.063f;         // -24 dB down
	float m_attackAlpha = 0.33f;
	float m_releaseAlpha = 0.04f;
	float m_noiseGateLinear = 58.0f;   // -55 dBFS
	std::unordered_map<uint16_t, AGCState> m_streams;
	mutable std::mutex m_mutex;
};
