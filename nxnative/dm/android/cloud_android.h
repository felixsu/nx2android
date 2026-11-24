// Cloud saves (cloud_android.c): the pause menu's Saves page uploads the game's save to the game library's server and downloads it from there
// (lixionary game-library apps/server/src/saves). The library writes DM_CLOUD_URL into env.txt at install (porter install --cloud-url): the game's own
// endpoint, https://<library>/api/saves/<TITLEID>. Everything here blocks: the menu calls it from a thread of its own
#pragma once
#include <stddef.h>
#include <stdint.h>

// where this device's save stands against the cloud (the library's decideSync, packages/shared/src/index.ts): local = the save's hash here, base = the
// cloud save this device last uploaded or downloaded (saves/cloud.txt), cloud = the newest cloud save
enum { DMC_BOTH_EMPTY, DMC_CLOUD_EMPTY, DMC_LOCAL_EMPTY, DMC_IN_SYNC, DMC_LOCAL_NEWER, DMC_CLOUD_NEWER, DMC_CONFLICT };
typedef struct { char id[96], device[64], platform[16], hash[65]; long long size; long long uploaded_ms; } dm_cloud_save;
typedef struct { int state, has_latest; dm_cloud_save latest; char local[65], base[65]; } dm_cloud_status;

static inline int dm_cloud_upload_asks(int s) { return s == DMC_CLOUD_NEWER || s == DMC_CONFLICT; }    // the cloud has a save this device has not seen
static inline int dm_cloud_download_asks(int s) { return s == DMC_LOCAL_NEWER || s == DMC_CONFLICT; }  // this device has progress the cloud has not seen

void dm_cloud_start(void *java_vm);   // ANativeActivity_onCreate: the JavaVM the menu's threads attach to for HTTP (java.net.HttpURLConnection)
const char *dm_cloud_url(void);   // DM_CLOUD_URL, NULL when cloud saves are off (an install without the library)
// the save's state: 0, or -1 and why (the server unreachable, ...)
int dm_cloud_check(dm_cloud_status *out, char *err, int ecap);
// upload the committed save (a snapshot through the save manager's export); saves/cloud.txt then names it. 0, or -1 and why
int dm_cloud_upload(dm_cloud_save *out, char *err, int ecap);
// download a cloud save (which: one dm_cloud_check listed) into saves/<name> (+ <name>.device) and ask for its import at the next start; the import
// (saves_android.c, request=cloud) backs up the current save as <stamp>-before-cloud and makes the download the sync base. 0, or -1 and why
int dm_cloud_download(const dm_cloud_save *which, char *name, int ncap, char *err, int ecap);

// ---- the parts with no network (tests/test_cloud_saves.py compiles them on the host) ----
// The save hash: SHA-256 over the files of save/ and save_device/ sorted by path, each fed as `path \0 size \0 bytes` (paths save/... and save_device/...;
// folders are not hashed). The same on the Mac, the server and here. 0 (hex "" when there are no files), or -errno
int dm_cloud_hash_tree(const char *save, const char *dev, char hex[65], long *files, long long *bytes);
// a ustar archive of the two trees (malloc'd), as the server reads it (apps/server/src/saves/archive.ts)
int dm_cloud_tar(const char *save, const char *dev, uint8_t **out, size_t *len, char *err, int ecap);
// an archive's save/ into dest_save and save_device/ into dest_dev (made here; they must not exist); *has_dev: whether it had a device save
int dm_cloud_untar(const uint8_t *buf, size_t len, const char *dest_save, const char *dest_dev, int *has_dev, char *err, int ecap);
int dm_cloud_decide(const char *local, const char *base, const char *cloud);
// a JSON response's field: a string's text (unescaped), or a number's or literal's text; from where `"key":` first appears in json. 0 when found
int dm_cloud_json(const char *json, const char *key, char *out, int cap);
