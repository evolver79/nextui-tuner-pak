// Downloads (HTTPS through ffmpeg's avio) with a file cache, and gzip.
#pragma once
#include <stddef.h>
// Fetches url into a malloc'd, NUL-terminated buffer (gunzipped if it was gzip). Uses `cache_path` when it's younger
// than max_age seconds, or when the download fails (offline). Returns NULL if neither works.
char *fetch(const char *url, const char *user_agent, const char *cache_path, long max_age, size_t *len);
// Downloads url straight to path (nothing kept in memory: guides can be hundreds of MB), unless path is younger than
// max_age. Gzip is kept as it is (load_xmltv reads both). Returns 1 when path is usable (fresh, downloaded, or an old
// copy when offline).
int fetch_to_file(const char *url, const char *user_agent, const char *path, long max_age);
// Same, in a background thread: returns at once. *done (if given) becomes 1 when the file has been downloaded.
void fetch_to_file_async(const char *url, const char *user_agent, const char *path, volatile int *done);
