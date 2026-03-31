#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <math.h>
#include <string.h>

#include <mp3dec.h>
#include <mp3common.h>

#include "mp3.h"
#include "libhelix-mp3/pub/mp3common.h"

LOG_MODULE_REGISTER(mp3, CONFIG_APP_LOG_LEVEL);

/* MP3 input buffer: large enough to hold >1 full frame (max ~1441 bytes) */
#define MP3_BUF_SIZE 4096

/* --- Header parsing tables --- */

/* MP3 bitrate table (kbps) indexed by bitrate_index (1..14), MPEG1 Layer3 */
static const uint32_t mp3_bitrate_table[16] = {
	0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
};

/* MP3 samplerate table indexed by samplerate_index (0..2), MPEG1 */
static const uint32_t mp3_samplerate_table[4] = {44100, 48000, 32000, 0};

/* Decode ID3v2 syncsafe size (4 bytes, 7 bits each) */
static uint32_t id3_syncsafe(const uint8_t *b)
{
	return ((uint32_t)b[0] << 21) |
	       ((uint32_t)b[1] << 14) |
	       ((uint32_t)b[2] << 7)  |
	       ((uint32_t)b[3]);
}

/* Parse ReplayGain from the LAME extension in a Xing/Info header.
 * buf points to the start of the first MP3 frame, len is bytes available.
 * channel_mode: 0-2 = stereo variants, 3 = mono (affects side info size). */
static int16_t lame_parse_replaygain(const uint8_t *buf, int len, uint8_t channel_mode)
{
	/* Side information size for MPEG1: 32 bytes stereo, 17 bytes mono */
	int side_info_size = (channel_mode == 3) ? 17 : 32;
	int xing_offset = 4 + side_info_size; /* frame header + side info */

	if (xing_offset + 4 > len) return 0;

	/* Check for Xing or Info tag */
	if (memcmp(buf + xing_offset, "Xing", 4) != 0 &&
	    memcmp(buf + xing_offset, "Info", 4) != 0) {
		return 0;
	}

	/* Parse Xing flags to find where the LAME tag starts */
	int pos = xing_offset + 4; /* past "Xing"/"Info" */
	if (pos + 4 > len) return 0;

	uint32_t xing_flags = ((uint32_t)buf[pos] << 24) | ((uint32_t)buf[pos + 1] << 16) |
			      ((uint32_t)buf[pos + 2] << 8) | buf[pos + 3];
	pos += 4;

	if (xing_flags & 0x01) pos += 4;   /* frames count */
	if (xing_flags & 0x02) pos += 4;   /* bytes count */
	if (xing_flags & 0x04) pos += 100; /* TOC */
	if (xing_flags & 0x08) pos += 4;   /* quality indicator */

	/* LAME tag: 9-byte version string, then extension data.
	 * ReplayGain fields at offset +15 (radio) and +17 (audiophile) from LAME tag start.
	 * Layout from LAME tag start:
	 *   +0:  9 bytes  encoder version string
	 *   +9:  1 byte   info tag revision / VBR method
	 *   +10: 1 byte   lowpass filter value
	 *   +11: 4 bytes  replay gain peak signal amplitude
	 *   +15: 2 bytes  radio (track) replay gain
	 *   +17: 2 bytes  audiophile (album) replay gain
	 */
	int lame_start = pos;
	if (lame_start + 19 > len) return 0;

	/* Verify it looks like a LAME tag (starts with "LAME" or "Lavf" or similar encoder) */
	/* We don't strictly require "LAME" — just check the gain fields are valid */

	/* Replay gain fields: 16 bits big-endian each
	 *   bits 15-13: name (001 = radio/track, 010 = audiophile/album)
	 *   bits 12-10: originator
	 *   bit 9:      sign (1 = negative)
	 *   bits 8-0:   absolute gain in tenths of dB */
	uint16_t album_rg = ((uint16_t)buf[lame_start + 17] << 8) | buf[lame_start + 18];
	uint8_t album_name = (album_rg >> 13) & 0x07;
	uint8_t album_sign = (album_rg >> 9) & 0x01;
	int16_t album_abs = album_rg & 0x01FF;

	/* Prefer album gain (name == 2), fall back to track gain (name == 1) */
	if (album_name == 2 && album_abs != 0) {
		return album_sign ? -album_abs : album_abs;
	}

	uint16_t track_rg = ((uint16_t)buf[lame_start + 15] << 8) | buf[lame_start + 16];
	uint8_t track_name = (track_rg >> 13) & 0x07;
	uint8_t track_sign = (track_rg >> 9) & 0x01;
	int16_t track_abs = track_rg & 0x01FF;

	if (track_name == 1 && track_abs != 0) {
		return track_sign ? -track_abs : track_abs;
	}

	return 0;
}

int mp3_parse_header(struct sdcard_audio_info *info)
{
	struct fs_file_t *f = &info->file;
	struct fs_dirent stat;
	uint8_t hdr[10];
	off_t data_start = 0;
	int rc;

	/* Read first 10 bytes to check for ID3v2 tag */
	rc = fs_read(f, hdr, 10);
	if (rc < 10) {
		return -EINVAL;
	}

	if (hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3') {
		/* ID3v2 tag present: skip it */
		uint32_t tag_size = id3_syncsafe(&hdr[6]);
		data_start = 10 + (off_t)tag_size;
		/* Check for extended footer (bit 4 of flags byte) */
		if (hdr[5] & 0x10) {
			data_start += 10;
		}
	}

	/* Seek to first MP3 frame */
	rc = fs_seek(f, data_start, FS_SEEK_SET);
	if (rc < 0) {
		return -EINVAL;
	}

	/* Read 4-byte frame header */
	uint8_t fh[4];
	rc = fs_read(f, fh, 4);
	if (rc < 4) {
		return -EINVAL;
	}

	/* Validate sync word (0xFFE0 = sync bits for MPEG audio) */
	if ((fh[0] != 0xFF) || ((fh[1] & 0xE0) != 0xE0)) {
		return -EINVAL;
	}

	/* MPEG version: bits 4-3 of byte 1; 0b11 = MPEG1 */
	uint8_t mpeg_version = (fh[1] >> 3) & 0x3;
	if (mpeg_version != 3) {
		/* Only MPEG1 supported */
		return -EINVAL;
	}

	/* Layer: bits 2-1 of byte 1; 0b01 = Layer3 */
	uint8_t layer = (fh[1] >> 1) & 0x3;
	if (layer != 1) {
		return -EINVAL;
	}

	/* Bitrate index: bits 7-4 of byte 2 */
	uint8_t br_idx = (fh[2] >> 4) & 0xF;
	if (br_idx == 0 || br_idx == 15) {
		return -EINVAL; /* free or bad */
	}
	uint32_t bitrate_bps = mp3_bitrate_table[br_idx] * 1000;

	/* Samplerate index: bits 3-2 of byte 2 */
	uint8_t sr_idx = (fh[2] >> 2) & 0x3;
	uint32_t samplerate = mp3_samplerate_table[sr_idx];
	if (samplerate == 0) {
		return -EINVAL;
	}

	/* Channel mode: bits 7-6 of byte 3; 3 = mono */
	uint8_t channel_mode = (fh[3] >> 6) & 0x3;
	uint16_t channels = (channel_mode == 3) ? 1 : 2;

	uint32_t avg_frame_bytes = (144 * bitrate_bps) / samplerate;
	uint32_t frame_bytes_num = 144 * bitrate_bps;

	int16_t replaygain_tenth_db = 0;

	/* Read the first frame to parse Xing/LAME header for ReplayGain */
	{
		uint8_t frame_buf[256];
		/* We already read 4 bytes of the frame header */
		memcpy(frame_buf, fh, 4);
		int to_read = sizeof(frame_buf) - 4;
		rc = fs_read(f, frame_buf + 4, to_read);
		if (rc > 0) {
			replaygain_tenth_db =
				lame_parse_replaygain(frame_buf, 4 + rc, channel_mode);
			if (replaygain_tenth_db != 0) {
				LOG_INF("ReplayGain: %s%d.%d dB",
					replaygain_tenth_db < 0 ? "-" : "",
					(replaygain_tenth_db < 0
						 ? -replaygain_tenth_db
						 : replaygain_tenth_db) / 10,
					(replaygain_tenth_db < 0
						 ? -replaygain_tenth_db
						 : replaygain_tenth_db) % 10);
			}
		}
	}

	/* Convert dB to Q15 linear multiplier */
	float gain_db = replaygain_tenth_db / 10.0f;
	info->rg_multiplier_q16 = (uint32_t)(powf(10.0f, gain_db / 20.0f) * 65536.0f);

	/* Estimate total samples from file size */
	rc = fs_stat(info->path, &stat);
	if (rc < 0) {
		return rc;
	}

	uint32_t audio_bytes = (uint32_t)(stat.size - data_start);
	uint32_t total_frames = audio_bytes / avg_frame_bytes;
	uint32_t total_samples = total_frames * 1152; /* 1152 samples/frame for MPEG1 */

	info->samplerate      = samplerate;
	info->channels        = channels;
	info->total_samples   = total_samples;
	info->data_start      = data_start;
	info->frame_bytes_num = frame_bytes_num;

	return 0;
}

/* --- Decoder state --- */

static HMP3Decoder mp3_dec;
static uint8_t mp3_buf[MP3_BUF_SIZE];
static int mp3_buf_len;
static struct sdcard_audio_info *cur_info;

/* Clear the bit reservoir so stale data from a previous file/position
 * does not corrupt decoding of the new stream. */
static void mp3_clear_bit_reservoir(void)
{
	MP3DecInfo *dec = (MP3DecInfo *)mp3_dec;
	memset(dec->mainBuf, 0, MAINBUF_SIZE);
	dec->mainDataBytes = 0;
}

/* Refill mp3_buf from file up to MP3_BUF_SIZE bytes */
static int mp3_buf_fill(void)
{
	int space = MP3_BUF_SIZE - mp3_buf_len;
	if (space <= 0) {
		return 0;
	}

	if (space < 512) return 0;

	int rc = fs_read(&cur_info->file, mp3_buf + mp3_buf_len, space);
	if (rc < 0) {
		LOG_WRN("SD read error: %d", rc);
		return rc;
	}
	if (rc == 0) {
		/* EOF — discard stale data and loop with a clean bit reservoir */
		mp3_buf_len = 0;
		mp3_clear_bit_reservoir();
		fs_seek(&cur_info->file, cur_info->data_start, FS_SEEK_SET);
		rc = fs_read(&cur_info->file, mp3_buf, MP3_BUF_SIZE);
		if (rc < 0) {
			LOG_WRN("SD read error after loop: %d", rc);
			return rc;
		}
		LOG_DBG("EOF loop: decoder reset");
	}
	mp3_buf_len += rc;
	return 0;
}

int mp3_decoder_init(void)
{
	mp3_dec = MP3InitDecoder();
	if (!mp3_dec) {
		LOG_ERR("MP3InitDecoder failed");
		return -ENOMEM;
	}
	return 0;
}

int mp3_switch_file(struct sdcard_audio_info *info, uint64_t global_sample_counter)
{
	uint32_t t_start = k_uptime_ticks();
	uint32_t t_seek, t_read;

	cur_info = info;

	/* Seek to position matching global_sample_counter */
	if (info->total_samples > 0) {
		uint32_t seek_sample = (uint32_t)(global_sample_counter % info->total_samples);
		uint32_t seek_frame = seek_sample / 1152;
		/* Rounded rational arithmetic: avoids floor-division accumulation error */
		off_t byte_offset =
			info->data_start + (off_t)(((uint64_t)seek_frame * info->frame_bytes_num +
						    info->samplerate / 2) /
						   info->samplerate);
		fs_seek(&info->file, byte_offset, FS_SEEK_SET);
	} else {
		fs_seek(&info->file, info->data_start, FS_SEEK_SET);
	}
	t_seek = k_uptime_ticks();

	/* Discard stale compressed data and clear the bit reservoir */
	mp3_buf_len = 0;
	mp3_clear_bit_reservoir();

	mp3_buf_fill();
	t_read = k_uptime_ticks();

	LOG_INF("switch_mp3 %s - gain %d: seek=%u us, read=%u us, total=%u us", info->path,
		info->rg_multiplier_q16,
		k_ticks_to_us_near32(t_seek - t_start), k_ticks_to_us_near32(t_read - t_seek),
		k_ticks_to_us_near32(t_read - t_start));

	return 0;
}

int16_t mp3_decode_frame(int16_t *buf)
{
	int err;
	int skipped_bytes = 0;
	int maindata_underflows = 0;
	int indata_underflows = 0;
	int other_errors = 0;

	uint32_t t0_us = k_ticks_to_us_near32(k_uptime_ticks());

	for (int attempts = 0; attempts < 16; attempts++) {
		/* Top up compressed buffer */
		mp3_buf_fill();

		/* Find next sync word */
		int offset = MP3FindSyncWord(mp3_buf, mp3_buf_len);
		if (offset < 0) {
			LOG_WRN("No sync");
			/* No sync found — discard buffer and retry */
			skipped_bytes += mp3_buf_len;
			mp3_buf_len = 0;
			continue;
		}
		if (offset > 0) {
			skipped_bytes += offset;
			memmove(mp3_buf, mp3_buf + offset, mp3_buf_len - offset);
			mp3_buf_len -= offset;
		}

		/* Decode one frame */
		unsigned char *read_ptr = mp3_buf;
		int bytes_left = mp3_buf_len;

		err = MP3Decode(mp3_dec, &read_ptr, &bytes_left, buf, 0);

		/* Update buffer: consume bytes that the decoder advanced past */
		int consumed = mp3_buf_len - bytes_left;
		if (consumed > 0) {
			memmove(mp3_buf, mp3_buf + consumed, bytes_left);
			mp3_buf_len = bytes_left;
		}

		if (err == ERR_MP3_NONE) {
			MP3FrameInfo fi;
			MP3GetLastFrameInfo(mp3_dec, &fi);

			uint32_t elapsed_us = k_ticks_to_us_near32(k_uptime_ticks()) - t0_us;
			if (skipped_bytes || maindata_underflows || indata_underflows ||
			    other_errors || elapsed_us > 10000) {
				// LOG_WRN("mp3_decode_frame: %u us, skipped=%d maindata_uflow=%d "
				// 	"indata_uflow=%d other_err=%d samps=%d",
				// 	elapsed_us, skipped_bytes, maindata_underflows,
				// 	indata_underflows, other_errors, fi.outputSamps);
			}
			return fi.outputSamps;
		} else if (err == ERR_MP3_MAINDATA_UNDERFLOW) {
			maindata_underflows++;
			continue;
		} else if (err == ERR_MP3_INDATA_UNDERFLOW) {
			indata_underflows++;
			continue;
		} else {
			LOG_ERR("MP3Decode: other err: %d", err);
			other_errors++;
			/* Sync error or bad frame: skip 1 byte and resync */
			if (mp3_buf_len > 0) {
				memmove(mp3_buf, mp3_buf + 1, mp3_buf_len - 1);
				mp3_buf_len--;
			}
		}
	}

	/* Fallback: output silence for this iteration */
	uint32_t elapsed_us = k_ticks_to_us_near32(k_uptime_ticks()) - t0_us;
	LOG_WRN("mp3_decode_frame: decode failed after 8 attempts in %u us, "
		"buf_len=%d skipped=%d maindata_uflow=%d indata_uflow=%d other_err=%d",
		elapsed_us, mp3_buf_len, skipped_bytes, maindata_underflows, indata_underflows,
		other_errors);
	return 0;
}
