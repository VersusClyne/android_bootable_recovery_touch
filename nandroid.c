#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sys/wait.h>
#include <sys/limits.h>
#include <dirent.h>
#include <sys/stat.h>

#include <signal.h>
#include <sys/wait.h>

#include "common.h"
#include "cutils/properties.h"
#include "install.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "recovery_ui.h"

#include <sys/vfs.h>

#include "extendedcommands.h"
#include "nandroid.h"
#include "mounts.h"

//#include "flashutils/flashutils.h"
#include <libgen.h>

void nandroid_generate_timestamp_path(const char* backup_path, const char* loc)
{
    time_t t = time(NULL);
    struct tm *tmp = localtime(&t);
    if (tmp == NULL)
    {
        struct timeval tp;
        gettimeofday(&tp, NULL);
        sprintf(backup_path, "%s/clockworkmod/backup/%d", loc, tp.tv_sec);
    }
    else
    {
		char tmps[PATH_MAX];
        strftime(tmps, PATH_MAX, "%F.%H.%M.%S", tmp);
		sprintf(backup_path, "%s/clockworkmod/backup/%s", loc, tmps);
    }
}

static int print_and_error(const char* message) {
    ui_print("%s", message);
    return 1;
}

static int yaffs_files_total = 0;
static int yaffs_files_count = 0;
static void yaffs_callback(const char* filename)
{
    if (filename == NULL)
        return;
    const char* justfile = basename(filename);
    char tmp[PATH_MAX];
    strcpy(tmp, justfile);
    if (tmp[strlen(tmp) - 1] == '\n')
        tmp[strlen(tmp) - 1] = NULL;
    if (strlen(tmp) < 30)
        ui_print("%s", tmp);
    yaffs_files_count++;
    if (yaffs_files_total != 0)
        ui_set_progress((float)yaffs_files_count / (float)yaffs_files_total);
    ui_reset_text_col();
}

static void compute_directory_stats(const char* directory)
{
    char tmp[PATH_MAX];
    sprintf(tmp, "find %s | wc -l > /tmp/dircount", directory);
    __system(tmp);
    char count_text[100];
    FILE* f = fopen("/tmp/dircount", "r");
    fread(count_text, 1, sizeof(count_text), f);
    fclose(f);
    yaffs_files_count = 0;
    yaffs_files_total = atoi(count_text);
    ui_reset_progress();
    ui_show_progress(1, 0);
}

typedef void (*file_event_callback)(const char* filename);
typedef int (*nandroid_backup_handler)(const char* backup_path, const char* backup_file_image, int callback);

static int tar_compress_wrapper(const char* backup_path, const char* backup_file_image, int callback) {
    char tmp[PATH_MAX];
    if (strcmp(backup_path, "/data") == 0 && volume_for_path("/sdcard") == NULL)
      sprintf(tmp, "cd $(dirname %s) ; tar cvf %s.tar --exclude 'media' $(basename %s) ; exit $?", backup_path, backup_file_image, backup_path);
    else
      sprintf(tmp, "cd $(dirname %s) ; tar cvf %s.tar $(basename %s) ; exit $?", backup_path, backup_file_image, backup_path);

    FILE *fp = __popen(tmp, "r");
    if (fp == NULL) {
        ui_print("Unable to execute tar.\n");
        return -1;
    }

    while (fgets(tmp, PATH_MAX, fp) != NULL) {
        tmp[PATH_MAX - 1] = NULL;
        if (callback)
            yaffs_callback(tmp);
    }

    return __pclose(fp);
}

static nandroid_backup_handler get_backup_handler(const char *backup_path) {
    Volume *v = volume_for_path(backup_path);
    if (v == NULL) {
        ui_print("Unable to find volume.\n");
        return NULL;
    }
    MountedVolume *mv = find_mounted_volume_by_mount_point(v->mount_point);
    if (mv == NULL) {
        ui_print("Unable to find mounted volume: %s\n", v->mount_point);
        return NULL;
    }

    if (strcmp(backup_path, "/data") == 0 && is_data_media()) {
        return tar_compress_wrapper;
    }

    return tar_compress_wrapper;
}


int nandroid_backup_partition_extended(const char* backup_path, const char* mount_point, int umount_when_finished) {
    int ret = 0;
    char* name = basename(mount_point);

    struct stat file_info;
    int callback = stat("/sdcard/clockworkmod/.hidenandroidprogress", &file_info) != 0;
    
    ui_print("Backing up %s...\n", name);
    if (0 != (ret = ensure_path_mounted(mount_point) != 0)) {
        ui_print("Can't mount %s!\n", mount_point);
        return ret;
    }
    compute_directory_stats(mount_point);
    char tmp[PATH_MAX];
    scan_mounted_volumes();
    Volume *v = volume_for_path(mount_point);
    MountedVolume *mv = NULL;
    if (v != NULL)
        mv = find_mounted_volume_by_mount_point(v->mount_point);
    if (mv == NULL || mv->filesystem == NULL)
        sprintf(tmp, "%s/%s.auto", backup_path, name);
    else
        sprintf(tmp, "%s/%s.%s", backup_path, name, mv->filesystem);
    nandroid_backup_handler backup_handler = get_backup_handler(mount_point);
    if (backup_handler == NULL) {
        ui_print("Error finding an appropriate backup handler.\n");
        return -2;
    }
    ret = backup_handler(mount_point, tmp, callback);
    if (umount_when_finished) {
        ensure_path_unmounted(mount_point);
    }
    if (0 != ret) {
        ui_print("Error while making a backup image of %s!\n", mount_point);
        return ret;
    }
    return 0;
}

int nandroid_backup_partition(const char* backup_path, const char* root) {
    Volume *vol = volume_for_path(root);
    // make sure the volume exists before attempting anything...
    if (vol == NULL || vol->fs_type == NULL)
        return NULL;

    return nandroid_backup_partition_extended(backup_path, root, 1);
}

int nandroid_adv_backup(const char* backup_path, int backup_pds, int backup_system, int backup_data, int backup_cache, int backup_webtop, int backup_preinstall) {
	ui_set_background(BACKGROUND_ICON_INSTALLING);

    if (ensure_path_mounted(backup_path) != 0) {
        return print_and_error("Can't mount backup path.\n");
    }
	Volume* volume = volume_for_path(backup_path);
	if (NULL == volume)
		return print_and_error("Unable to find volume for backup path.\n");
	int ret;
	    struct statfs s;
    if (NULL != volume) {
        if (0 != (ret = statfs(volume->mount_point, &s)))
            return print_and_error("Unable to stat backup path.\n");
        uint64_t bavail = s.f_bavail;
        uint64_t bsize = s.f_bsize;
        uint64_t sdcard_free = bavail * bsize;
        uint64_t sdcard_free_mb = sdcard_free / (uint64_t)(1024 * 1024);
        ui_print("Free space : %s - %lluMB\n", volume->mount_point, sdcard_free_mb);
        if (sdcard_free_mb < 150)
            ui_print("There may not be enough free space to complete backup... continuing...\n");
    }
	ui_print("Backup path : %s\n\n",backup_path);

    char tmp[PATH_MAX];
    sprintf(tmp, "mkdir -p %s", backup_path);
    __system(tmp);

    if (backup_pds && 0 != (ret = nandroid_backup_partition(backup_path, "/pds")))
        return ret;

    if (backup_system && 0 != (ret = nandroid_backup_partition(backup_path, "/system")))
        return ret;

    if (backup_data && 0 != (ret = nandroid_backup_partition(backup_path, "/data")))
        return ret;

	if (backup_data) {
    	if (0 != stat("/emmc/.android_secure", &s)) {
        	ui_print("No /emmc/.android_secure found. Skipping backup of applications on external storage.\n");
    	} else {
    	    if (0 != (ret = nandroid_backup_partition_extended(backup_path, "/emmc/.android_secure", 0)))
    	        return ret;
    	}
	}

    if (backup_cache && 0 != (ret = nandroid_backup_partition_extended(backup_path, "/cache", 0)))
        return ret;

    if (backup_webtop && 0 != (ret = nandroid_backup_partition_extended(backup_path, "/webtop", 0)))
        return ret;

    if (backup_preinstall && 0 != (ret = nandroid_backup_partition_extended(backup_path, "/preinstall", 0)))
        return ret;

    ui_print("Generating md5 sum...\n");
    sprintf(tmp, "nandroid-md5.sh %s", backup_path);
    if (0 != (ret = __system(tmp))) {
        ui_print("Error while generating md5 sum!\n");
        return ret;
    }
    
    sync();
    ui_set_background(BACKGROUND_ICON_NONE);
    ui_reset_progress();
    ui_print("\nBackup complete!\n");
    return 0;
}

int nandroid_backups(const char* loc) {
	if (!strcmp(recovery_mode, "stock")) {
		return nandroid_adv_backup(loc, 1, 1, 1, 1, 1, 1);
	} else {
		return nandroid_adv_backup(loc, 1, 1, 1, 1, 0, 1);
	}
}

int nandroid_backup(const char* backup_path) {
   return nandroid_adv_backup(backup_path, 0, 1, 1, 1, 0, 0);
}

typedef int (*format_function)(char* root);

static void ensure_directory(const char* dir) {
    char tmp[PATH_MAX];
    sprintf(tmp, "mkdir -p %s", dir);
    __system(tmp);
}

typedef int (*nandroid_restore_handler)(const char* backup_file_image, const char* backup_path, int callback);

static int tar_extract_wrapper(const char* backup_file_image, const char* backup_path, int callback) {
    char tmp[PATH_MAX];
    sprintf(tmp, "cd $(dirname %s) ; tar xvf %s ; exit $?", backup_path, backup_file_image);

    char path[PATH_MAX];
    FILE *fp = __popen(tmp, "r");
    if (fp == NULL) {
        ui_print("Unable to execute tar.\n");
        return -1;
    }

    while (fgets(path, PATH_MAX, fp) != NULL) {
        if (callback)
            yaffs_callback(path);
    }

    return __pclose(fp);
}

static nandroid_restore_handler get_restore_handler(const char *backup_path) {
    Volume *v = volume_for_path(backup_path);
    if (v == NULL) {
        ui_print("Unable to find volume.\n");
        return NULL;
    }
    scan_mounted_volumes();
    MountedVolume *mv = find_mounted_volume_by_mount_point(v->mount_point);
    if (mv == NULL) {
        ui_print("Unable to find mounted volume: %s\n", v->mount_point);
        return NULL;
    }

    if (strcmp(backup_path, "/data") == 0 && is_data_media()) {
        return tar_extract_wrapper;
    }

    return tar_extract_wrapper;
}

int nandroid_restore_partition_extended(const char* backup_path, const char* mount_point, int umount_when_finished) {
    int ret = 0;
    char* name = basename(mount_point);

    nandroid_restore_handler restore_handler = NULL;
    const char *filesystems[] = { "yaffs2", "ext2", "ext3", "ext4", "vfat", "rfs", NULL };
    const char* backup_filesystem = NULL;
    Volume *vol = volume_for_path(mount_point);
    const char *device = NULL;
    if (vol != NULL)
        device = vol->device;

    char tmp[PATH_MAX];
    sprintf(tmp, "%s/%s.img", backup_path, name);
    struct stat file_info;
    if (0 != (ret = statfs(tmp, &file_info))) {
        // can't find the backup, it may be the new backup format?
        // iterate through the backup types
        printf("couldn't find default\n");
        char *filesystem;
        int i = 0;
        while ((filesystem = filesystems[i]) != NULL) {
            sprintf(tmp, "%s/%s.%s.tar", backup_path, name, filesystem);
            if (0 == (ret = statfs(tmp, &file_info))) {
                backup_filesystem = filesystem;
                restore_handler = tar_extract_wrapper;
                break;
            }
            i++;
        }

        if (backup_filesystem == NULL || restore_handler == NULL) {
            ui_print("%s.img not found. Skipping restore of %s.\n", name, mount_point);
            return 0;
        }
        else {
            printf("Found new backup image: %s\n", tmp);
        }

        // If the fs_type of this volume is "auto" or mount_point is /data
        // and is_data_media (redundantly, and vol for /sdcard is NULL), let's revert
        // to using a rm -rf, rather than trying to do a
        // ext3/ext4/whatever format.
        // This is because some phones (like DroidX) will freak out if you
        // reformat the /system or /data partitions, and not boot due to
        // a locked bootloader.
        // Other devices, like the Galaxy Nexus, XOOM, and Galaxy Tab 10.1
        // have a /sdcard symlinked to /data/media. /data is set to "auto"
        // so that when the format occurs, /data/media is not erased.
        // The "auto" fs type preserves the file system, and does not
        // trigger that lock.
        // Or of volume does not exist (.android_secure), just rm -rf.
        if (vol == NULL || 0 == strcmp(vol->fs_type, "auto"))
            backup_filesystem = NULL;
        else if (0 == strcmp(vol->mount_point, "/data") && volume_for_path("/sdcard") == NULL && is_data_media())
	         backup_filesystem = NULL;
    }

    ensure_directory(mount_point);

    int callback = stat("/sdcard/clockworkmod/.hidenandroidprogress", &file_info) != 0;

    ui_print("Restoring %s...\n", name);
    if (backup_filesystem == NULL) {
        if (0 != (ret = format_volume(mount_point))) {
            ui_print("Error while erasing %s!\n", mount_point);
            return ret;
        }
    }
    else if (0 != (ret = format_device(device, mount_point, backup_filesystem))) {
        ui_print("Error while formatting %s!\n", mount_point);
        return ret;
    }

    if (0 != (ret = ensure_path_mounted(mount_point))) {
        ui_print("Can't mount %s!\n", mount_point);
        return ret;
    }

    if (restore_handler == NULL)
        restore_handler = get_restore_handler(mount_point);
    if (restore_handler == NULL) {
        ui_print("Error finding an appropriate restore handler.\n");
        return -2;
    }
    if (0 != (ret = restore_handler(tmp, mount_point, callback))) {
        ui_print("Error while restoring %s!\n", mount_point);
        return ret;
    }

    if (umount_when_finished) {
        ensure_path_unmounted(mount_point);
    }
    
    return 0;
}

int nandroid_restore_partition(const char* backup_path, const char* root) {
    Volume *vol = volume_for_path(root);
    // make sure the volume exists...
    if (vol == NULL || vol->fs_type == NULL)
        return 0;

    return nandroid_restore_partition_extended(backup_path, root, 1);
}

int nandroid_restore(const char* backup_path, int restore_pds, int restore_system, int restore_data, int restore_cache, int restore_webtop, int restore_preinstall)
{
    ui_set_background(BACKGROUND_ICON_INSTALLING);
    ui_show_indeterminate_progress();
    yaffs_files_total = 0;

    if (ensure_path_mounted(backup_path) != 0)
        return print_and_error("Can't mount backup path\n");
    
    char tmp[PATH_MAX];

    ui_print("Checking MD5 sums...\n");
    sprintf(tmp, "cd %s && md5sum -c nandroid.md5", backup_path);
    if (0 != __system(tmp))
        return print_and_error("MD5 mismatch!\n");
    
    int ret;

    if (restore_pds && 0 != (ret = nandroid_restore_partition(backup_path, "/pds")))
        return ret;
    
    if (restore_system && 0 != (ret = nandroid_restore_partition(backup_path, "/system")))
        return ret;

    if (restore_data && 0 != (ret = nandroid_restore_partition(backup_path, "/data")))
        return ret;
        
    if (has_datadata()) {
        if (restore_data && 0 != (ret = nandroid_restore_partition(backup_path, "/datadata")))
            return ret;
    }

    if (restore_data && 0 != (ret = nandroid_restore_partition_extended(backup_path, "/emmc/.android_secure", 0)))
        return ret;

    if (restore_cache && 0 != (ret = nandroid_restore_partition_extended(backup_path, "/cache", 0)))
        return ret;

    if (restore_webtop && 0 != (ret = nandroid_restore_partition(backup_path, "/webtop")))
        return ret;

	if (restore_preinstall && 0 != (ret = nandroid_restore_partition(backup_path, "/preinstall")))
	    return ret;

    sync();
    ui_set_background(BACKGROUND_ICON_NONE);
    ui_reset_progress();
    ui_print("\nRestore complete!\n");
    return 0;
}

int nandroid_usage()
{
    printf("Usage: nandroid backup\n");
    printf("Usage: nandroid restore <directory>\n");
    return 1;
}

int nandroid_main(int argc, char** argv)
{
    if (argc > 3 || argc < 2)
        return nandroid_usage();
    
    if (strcmp("backup", argv[1]) == 0)
    {
        if (argc != 2)
            return nandroid_usage();
        
        char backup_path[PATH_MAX];
        nandroid_generate_timestamp_path(backup_path,"/sdcard");
        return nandroid_backup(backup_path);
    }

    if (strcmp("restore", argv[1]) == 0)
    {
        if (argc != 3)
            return nandroid_usage();

        return nandroid_restore(argv[2], 0, 1, 1, 1, 0, 0);
    }
    
    return nandroid_usage();
}
