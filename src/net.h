// Downloads (HTTPS through ffmpeg's avio) with a file cache, and gzip.
#pragma once
#include <stddef.h>
// Fetches url into a malloc'd, NUL-terminated buffer (gunzipped if it was gzip). Uses `cache_path` when it's younger
// than max_age seconds, or when the download fails (offline). Returns NULL if neither works.
char *fetch(const char *url, const char *user_agent, const char *cache_path, long max_age, size_t *len);
