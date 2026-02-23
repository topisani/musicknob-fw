#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <string.h>
#include <stdio.h>

#include "sdcard.h"

LOG_MODULE_REGISTER(sdcard, CONFIG_APP_LOG_LEVEL);

#define MAX_AUDIO_FILES 100

static struct sdcard_audio_info audio_infos[MAX_AUDIO_FILES];
static int audio_file_count;

#define AUTOMOUNT_NODE DT_NODELABEL(ffs1)
FS_FSTAB_DECLARE_ENTRY(AUTOMOUNT_NODE);

/* MP3 bitrate table (kbps) indexed by bitrate_index (1..14), MPEG1 Layer3 */
static const uint32_t mp3_bitrate_table[16] = {
	0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
};

/* MP3 samplerate table indexed by samplerate_index (0..2), MPEG1 */
static const uint32_t mp3_samplerate_table[4] = {44100, 48000, 32000, 0};

static bool has_mp3_ext(const char *name)
{
	size_t len = strlen(name);
	if (len < 4) {
		return false;
	}
	const char *ext = name + len - 4;
	return ext[0] == '.' &&
	       (ext[1] == 'm' || ext[1] == 'M') &&
	       (ext[2] == 'p' || ext[2] == 'P') &&
	       (ext[3] == '3' || ext[3] == '3');
}

/* Decode ID3v2 syncsafe size (4 bytes, 7 bits each) */
static uint32_t id3_syncsafe(const uint8_t *b)
{
	return ((uint32_t)b[0] << 21) |
	       ((uint32_t)b[1] << 14) |
	       ((uint32_t)b[2] << 7)  |
	       ((uint32_t)b[3]);
}

static int read_mp3_info(const char *path, struct sdcard_audio_info *info)
{
	struct fs_file_t f;
	struct fs_dirent stat;
	uint8_t hdr[10];
	off_t data_start = 0;
	int rc;

	fs_file_t_init(&f);
	rc = fs_open(&f, path, FS_O_READ);
	if (rc < 0) {
		return rc;
	}

	/* Read first 10 bytes to check for ID3v2 tag */
	rc = fs_read(&f, hdr, 10);
	if (rc < 10) {
		goto err;
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
	rc = fs_seek(&f, data_start, FS_SEEK_SET);
	if (rc < 0) {
		goto err;
	}

	/* Read 4-byte frame header */
	uint8_t fh[4];
	rc = fs_read(&f, fh, 4);
	if (rc < 4) {
		goto err;
	}

	/* Validate sync word (0xFFE0 = sync bits for MPEG audio) */
	if ((fh[0] != 0xFF) || ((fh[1] & 0xE0) != 0xE0)) {
		goto err;
	}

	/* MPEG version: bits 4-3 of byte 1; 0b11 = MPEG1 */
	uint8_t mpeg_version = (fh[1] >> 3) & 0x3;
	if (mpeg_version != 3) {
		/* Only MPEG1 supported */
		goto err;
	}

	/* Layer: bits 2-1 of byte 1; 0b01 = Layer3 */
	uint8_t layer = (fh[1] >> 1) & 0x3;
	if (layer != 1) {
		goto err;
	}

	/* Bitrate index: bits 7-4 of byte 2 */
	uint8_t br_idx = (fh[2] >> 4) & 0xF;
	if (br_idx == 0 || br_idx == 15) {
		goto err; /* free or bad */
	}
	uint32_t bitrate_bps = mp3_bitrate_table[br_idx] * 1000;

	/* Samplerate index: bits 3-2 of byte 2 */
	uint8_t sr_idx = (fh[2] >> 2) & 0x3;
	uint32_t samplerate = mp3_samplerate_table[sr_idx];
	if (samplerate == 0) {
		goto err;
	}

	/* Channel mode: bits 7-6 of byte 3; 3 = mono */
	uint8_t channel_mode = (fh[3] >> 6) & 0x3;
	uint16_t channels = (channel_mode == 3) ? 1 : 2;

	uint32_t avg_frame_bytes = (144 * bitrate_bps) / samplerate;
	uint32_t frame_bytes_num = 144 * bitrate_bps;

	/* Estimate total samples from file size */
	fs_close(&f);
	rc = fs_stat(path, &stat);
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

err:
	fs_close(&f);
	return -EINVAL;
}

static void enumerate_mp3_files(const char *path)
{
	struct fs_dir_t dir;
	struct fs_dirent entry;
	char filepath[256];

	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, path) < 0) {
		LOG_ERR("Failed to open directory %s", path);
		return;
	}

	audio_file_count = 0;

	while (fs_readdir(&dir, &entry) == 0 && entry.name[0] != '\0') {
		if (entry.type != FS_DIR_ENTRY_FILE) {
			continue;
		}
		if (!has_mp3_ext(entry.name)) {
			continue;
		}

		snprintf(filepath, sizeof(filepath), "%s/%s", path, entry.name);

		if (audio_file_count >= MAX_AUDIO_FILES) {
			LOG_WRN("Too many MP3 files, ignoring %s", entry.name);
			continue;
		}

		struct sdcard_audio_info *info = &audio_infos[audio_file_count];
		strncpy(info->path, filepath, sizeof(info->path) - 1);
		info->path[sizeof(info->path) - 1] = '\0';

		if (read_mp3_info(filepath, info) == 0) {
			uint32_t duration_s = info->total_samples / info->samplerate;
			LOG_INF("%s: %u Hz, %u ch, %u kbps, %u:%02u",
				entry.name,
				info->samplerate,
				info->channels,
				info->frame_bytes_num / (144 * 1000),
				duration_s / 60,
				duration_s % 60);
			audio_file_count++;
		} else {
			audio_file_count++;
			LOG_WRN("%s: failed to parse MP3 header", entry.name);
		}
	}

	fs_closedir(&dir);

	/* Sort alphabetically by path */
	for (int i = 1; i < audio_file_count; i++) {
		struct sdcard_audio_info tmp = audio_infos[i];
		int j = i - 1;
		while (j >= 0 && strcmp(audio_infos[j].path, tmp.path) > 0) {
			audio_infos[j + 1] = audio_infos[j];
			j--;
		}
		audio_infos[j + 1] = tmp;
	}
}

int sdcard_init(void)
{
	struct fs_mount_t *mp = &FS_FSTAB_ENTRY(AUTOMOUNT_NODE);
	struct fs_statvfs stat;

	if (fs_statvfs(mp->mnt_point, &stat) != 0) {
		LOG_ERR("SD card not mounted at %s", mp->mnt_point);
		return -EIO;
	}

	LOG_INF("SD card mounted at %s (%lu KB free)",
		mp->mnt_point,
		(unsigned long)stat.f_bfree * stat.f_frsize / 1024);

	enumerate_mp3_files(mp->mnt_point);
	return 0;
}

int sdcard_get_audio_count(void)
{
	return audio_file_count;
}

const struct sdcard_audio_info *sdcard_get_audio_info(int index)
{
	if (index < 0 || index >= audio_file_count) {
		return NULL;
	}
	return &audio_infos[index];
}
