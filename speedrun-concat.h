#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Concatenates two media files (e.g. pre-buffer + main run) without re-encoding.
 * Streams are matched and copied packet by packet, seamlessly stitching timestamps.
 *
 * Returns true on success, false on failure.
 */
bool speedrun_concat_files(const char *in_pre, const char *in_main, const char *out_final);

typedef void (*speedrun_concat_callback)(bool success, const char *out_final, void *param);

/*
 * Asynchronously concatenates two media files on a background thread.
 * If delete_sources_on_success is true, in_pre and in_main are unlinked upon success.
 */
void speedrun_concat_files_async(const char *in_pre, const char *in_main, const char *out_final,
				 bool delete_sources_on_success, speedrun_concat_callback callback, void *param);

#ifdef __cplusplus
}
#endif
