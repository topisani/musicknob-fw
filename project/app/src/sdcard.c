#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <string.h>
#include <stdio.h>

#include "sdcard.h"
#include "mp3.h"

LOG_MODULE_REGISTER(sdcard, CONFIG_APP_LOG_LEVEL);

#define MAX_AUDIO_FILES 100

static struct sdcard_audio_info audio_infos[MAX_AUDIO_FILES];
static int audio_file_count;

#define AUTOMOUNT_NODE DT_NODELABEL(ffs1)
FS_FSTAB_DECLARE_ENTRY(AUTOMOUNT_NODE);

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

static void enumerate_audio_files(const char *path)
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

		fs_file_t_init(&info->file);
		int rc = fs_open(&info->file, filepath, FS_O_READ);
		if (rc < 0) {
			LOG_ERR("%s: failed to open: %d", entry.name, rc);
			continue;
		}

		if (mp3_parse_header(info) == 0) {
			uint32_t duration_s = info->total_samples / info->samplerate;
			LOG_INF("%s: %u Hz, %u ch, %u kbps, %u:%02u",
				entry.name,
				info->samplerate,
				info->channels,
				info->frame_bytes_num / (144 * 1000),
				duration_s / 60,
				duration_s % 60);
		} else {
			LOG_WRN("%s: failed to parse MP3 header", entry.name);
		}

		fs_seek(&info->file, info->data_start, FS_SEEK_SET);
		audio_file_count++;
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

	enumerate_audio_files(mp->mnt_point);
	return 0;
}

int sdcard_get_audio_count(void)
{
	return audio_file_count;
}

struct sdcard_audio_info *sdcard_get_audio_info(int index)
{
	if (index < 0 || index >= audio_file_count) {
		return NULL;
	}
	return &audio_infos[index];
}
