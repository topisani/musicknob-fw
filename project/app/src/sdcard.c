#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <string.h>

#include "sdcard.h"

LOG_MODULE_REGISTER(sdcard, CONFIG_APP_LOG_LEVEL);

#define MAX_WAV_FILES 100

static struct sdcard_wav_info wav_infos[MAX_WAV_FILES];
static int wav_file_count;

#define AUTOMOUNT_NODE DT_NODELABEL(ffs1)
FS_FSTAB_DECLARE_ENTRY(AUTOMOUNT_NODE);

#define WAV_ID(s) \
	(uint32_t)(s[0] | (s[1] << 8) | (s[2] << 16) | (s[3] << 24))

#define WAV_RIFF_ID WAV_ID("RIFF")
#define WAV_WAVE_ID WAV_ID("WAVE")
#define WAV_FMT_ID  WAV_ID("fmt ")
#define WAV_DATA_ID WAV_ID("data")

struct wav_file {
	uint32_t type_id;
	uint32_t size;
	uint32_t fmt_id;
};

struct wav_format {
	uint32_t id;
	uint32_t size;
	uint16_t fmt;
	uint16_t channels;
	uint32_t samplerate;
	uint32_t byterate;
	uint16_t framesize;
	uint16_t bitdepth;
};

struct wav_data {
	uint32_t id;
	uint32_t size;
};

static bool has_wav_ext(const char *name)
{
	size_t len = strlen(name);
	if (len < 4) {
		return false;
	}
	const char *ext = name + len - 4;
	return ext[0] == '.' &&
	       (ext[1] == 'w' || ext[1] == 'W') &&
	       (ext[2] == 'a' || ext[2] == 'A') &&
	       (ext[3] == 'v' || ext[3] == 'V');
}

static int read_wav_header(const char *path, struct sdcard_wav_info *info)
{
	struct fs_file_t f;
	struct wav_file file;
	struct wav_format fmt;
	struct wav_data data;
	int rc;

	fs_file_t_init(&f);
	rc = fs_open(&f, path, FS_O_READ);
	if (rc < 0) {
		return rc;
	}

	rc = fs_read(&f, &file, sizeof(file));
	if (rc < (int)sizeof(file)) {
		goto err;
	}
	if (file.type_id != WAV_RIFF_ID || file.fmt_id != WAV_WAVE_ID) {
		goto err;
	}

	rc = fs_read(&f, &fmt, sizeof(fmt));
	if (rc < (int)sizeof(fmt)) {
		goto err;
	}
	if (fmt.id != WAV_FMT_ID) {
		goto err;
	}

	/* Skip any extra format bytes */
	if (fmt.size > sizeof(fmt) - 8) {
		fs_seek(&f, fmt.size - (sizeof(fmt) - 8), FS_SEEK_CUR);
	}

	/* Find the data chunk (skip non-data chunks) */
	while (true) {
		rc = fs_read(&f, &data, sizeof(data));
		if (rc < (int)sizeof(data)) {
			goto err;
		}
		if (data.id == WAV_DATA_ID) {
			break;
		}
		fs_seek(&f, data.size, FS_SEEK_CUR);
	}

	info->samplerate = fmt.samplerate;
	info->channels   = fmt.channels;
	info->bitdepth   = fmt.bitdepth;
	info->framesize  = fmt.framesize;
	info->data_size  = data.size;
	info->nframes    = data.size / fmt.framesize;
	info->data_start = fs_tell(&f);

	fs_close(&f);
	return 0;

err:
	fs_close(&f);
	return -EINVAL;
}

static void enumerate_wav_files(const char *path)
{
	struct fs_dir_t dir;
	struct fs_dirent entry;
	char filepath[64];

	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, path) < 0) {
		LOG_ERR("Failed to open directory %s", path);
		return;
	}

	wav_file_count = 0;

	while (fs_readdir(&dir, &entry) == 0 && entry.name[0] != '\0') {
		if (entry.type != FS_DIR_ENTRY_FILE) {
			continue;
		}
		if (!has_wav_ext(entry.name)) {
			continue;
		}

		snprintf(filepath, sizeof(filepath), "%s/%s", path, entry.name);

		if (wav_file_count >= MAX_WAV_FILES) {
			LOG_WRN("Too many WAV files, ignoring %s", entry.name);
			continue;
		}

		struct sdcard_wav_info *info = &wav_infos[wav_file_count];
		strncpy(info->path, filepath, sizeof(info->path) - 1);
		info->path[sizeof(info->path) - 1] = '\0';

		if (read_wav_header(filepath, info) == 0) {
			uint32_t duration_s = info->nframes / info->samplerate;
			LOG_INF("%s: %u Hz, %u ch, %u bit, %u:%02u",
				entry.name,
				info->samplerate,
				info->channels,
				info->bitdepth,
				duration_s / 60,
				duration_s % 60);
			wav_file_count++;
		} else {
			LOG_WRN("%s: failed to parse WAV header", entry.name);
		}
	}

	fs_closedir(&dir);

	/* Sort alphabetically by path */
	for (int i = 1; i < wav_file_count; i++) {
		struct sdcard_wav_info tmp = wav_infos[i];
		int j = i - 1;
		while (j >= 0 && strcmp(wav_infos[j].path, tmp.path) > 0) {
			wav_infos[j + 1] = wav_infos[j];
			j--;
		}
		wav_infos[j + 1] = tmp;
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

	enumerate_wav_files(mp->mnt_point);
	return 0;
}

int sdcard_get_wav_count(void)
{
	return wav_file_count;
}

const struct sdcard_wav_info *sdcard_get_wav_info(int index)
{
	if (index < 0 || index >= wav_file_count) {
		return NULL;
	}
	return &wav_infos[index];
}
