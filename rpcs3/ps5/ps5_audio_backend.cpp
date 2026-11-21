#include "stdafx.h"
#include "ps5_audio_backend.h"
#include "ps5_sce.h"

#include <chrono>
#include <cmath>

LOG_CHANNEL(ps5_audio_log, "SceAudioOut");

ps5_audio_backend::ps5_audio_backend()
{
	// sceAudioOutInit may be called more than once; anything but a failure is fine
	if (const int res = sceAudioOutInit(); res < 0 && res != static_cast<int>(0x8026000e))
	{
		ps5_audio_log.error("sceAudioOutInit() failed: 0x%x", res);
		return;
	}

	m_initialized = true;
}

ps5_audio_backend::~ps5_audio_backend()
{
	Close();
}

bool ps5_audio_backend::Open(std::string_view /*dev_id*/, AudioFreq freq, AudioSampleSize sample_size, AudioChannelCnt ch_cnt, audio_channel_layout layout)
{
	if (!m_initialized)
		return false;

	Close();
	std::lock_guard lock{m_cb_mutex};

	if (freq != AudioFreq::FREQ_48K)
	{
		ps5_audio_log.error("Only 48 kHz output is supported (requested %u Hz)", static_cast<u32>(freq));
		return false;
	}

	m_sampling_rate = freq;
	m_sample_size = sample_size;

	// The port is stereo; RPCS3 downmixes 5.1/7.1 to it
	setup_channel_layout(static_cast<u32>(ch_cnt), 2, layout, ps5_audio_log);

	const bool s16 = get_convert_to_s16();
	const u32 format = s16 ? ps5::AUDIO_OUT_S16_STEREO : ps5::AUDIO_OUT_FLOAT_STEREO;

	m_port = sceAudioOutOpen(ps5::AUDIO_OUT_USER_SYSTEM, 0, 0, grain, 48000, format);

	if (m_port < 0)
	{
		ps5_audio_log.error("sceAudioOutOpen() failed: 0x%x", m_port);
		m_port = -1;
		return false;
	}

	m_buffer.assign(grain * get_channels() * get_sample_size(), 0);
	ps5_audio_log.notice("Opened port %d: %s stereo, grain %u", m_port, s16 ? "s16" : "float", grain);

	m_stop = false;
	m_thread = std::make_unique<named_thread<std::function<void()>>>("SceAudioOut", [this]() { output_loop(); });
	return true;
}

void ps5_audio_backend::Close()
{
	if (m_thread)
	{
		m_stop = true;
		m_thread.reset(); // joins
	}

	std::lock_guard lock{m_cb_mutex};

	if (m_port >= 0)
	{
		sceAudioOutClose(m_port);
		m_port = -1;
	}

	m_playing = false;
}

void ps5_audio_backend::Play()
{
	std::lock_guard lock{m_cb_mutex};
	m_playing = true;
}

void ps5_audio_backend::Pause()
{
	std::lock_guard lock{m_cb_mutex};
	m_playing = false;
}

void ps5_audio_backend::output_loop()
{
	while (!m_stop && thread_ctrl::state() != thread_state::aborting)
	{
		u32 written = 0;
		const u32 bytes = ::size32(m_buffer);

		{
			std::unique_lock lock(m_cb_mutex, std::defer_lock);

			if (lock.try_lock_for(std::chrono::microseconds{50}) && m_write_callback && m_playing)
			{
				written = std::min(m_write_callback(bytes, m_buffer.data()), bytes);
			}
		}

		// Silence for what the callback did not fill
		std::memset(m_buffer.data() + written, 0, bytes - written);

		// BLZ: proof that sound really plays - level of what goes to the port, every 5 s
		{
			using clk = std::chrono::steady_clock;
			static thread_local clk::time_point lvl_start = clk::now();
			static thread_local double lvl_sq = 0.0, lvl_peak = 0.0;
			static thread_local u64 lvl_samples = 0, lvl_bytes = 0, lvl_filled = 0, lvl_grains = 0, lvl_errors = 0;
			if (get_convert_to_s16())
			{
				const s16* smp = reinterpret_cast<const s16*>(m_buffer.data());
				for (u32 i = 0; i < written / 2; i++)
				{
					const double v = smp[i] / 32768.0;
					lvl_sq += v * v;
					lvl_peak = std::max(lvl_peak, std::fabs(v));
				}
				lvl_samples += written / 2;
			}
			else
			{
				const f32* smp = reinterpret_cast<const f32*>(m_buffer.data());
				for (u32 i = 0; i < written / 4; i++)
				{
					const double v = smp[i];
					lvl_sq += v * v;
					lvl_peak = std::max(lvl_peak, std::fabs(v));
				}
				lvl_samples += written / 4;
			}
			lvl_bytes += bytes;
			lvl_filled += written;
			lvl_grains++;
			const int res = sceAudioOutOutput(m_port, m_buffer.data());
			if (res < 0) lvl_errors++;
			const auto now = clk::now();
			if (now - lvl_start >= std::chrono::seconds(5))
			{
				const double rms = lvl_samples ? std::sqrt(lvl_sq / lvl_samples) : 0.0;
				ps5_audio_log.notice("BLZ_AUDIO: rms_dbfs=%.1f peak=%.3f filled=%.0f%% grains=%llu out_errors=%llu",
					rms > 0 ? 20.0 * std::log10(rms) : -999.0, lvl_peak, lvl_bytes ? 100.0 * lvl_filled / lvl_bytes : 0.0,
					lvl_grains, lvl_errors);
				lvl_start = now; lvl_sq = 0; lvl_peak = 0; lvl_samples = lvl_bytes = lvl_filled = lvl_grains = lvl_errors = 0;
			}
		}
	}

	// Flush: an output of null waits for the port to drain
	sceAudioOutOutput(m_port, nullptr);
}
