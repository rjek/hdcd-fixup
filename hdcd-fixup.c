/* hdcd-fixup, a hack
 * Copyright 2020 Rob Kendrick - Licenced MIT.
 */

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 500
#endif

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sndfile.h>
#include <hdcd/hdcd_simple.h>

const int VER_MAJOR = 1;
const int VER_MINOR = 0;

static int opt_verbose = 0;
static bool opt_backup = true;
static bool opt_scan_only = false;

#define VERBLOG(lvl, fmt, ...) if (opt_verbose >= lvl) fprintf(stderr, fmt, __VA_ARGS__)

struct int_to_str {
    int key;
    const char *val;
};

static const struct int_to_str majorfmt_to_string[] = {
    { SF_FORMAT_WAV,    "Microsoft WAV" },
    { SF_FORMAT_AIFF,   "Apple AIFF" },
    { SF_FORMAT_AU,     "Sun AU" },
    { SF_FORMAT_W64,    "Sonic Foundry WAV64" },
    { SF_FORMAT_WAVEX,  "Microsoft WAV Extended" },
    { SF_FORMAT_FLAC,   "FLAC" },
    { SF_FORMAT_OGG,    "Ogg Container" },
    { SF_FORMAT_RF64,   "RF64 WAV" },
};
const size_t majorfmt_count = (sizeof(majorfmt_to_string) / sizeof(majorfmt_to_string[0]));

static const struct int_to_str minorfmt_to_string[] = {
    { SF_FORMAT_PCM_S8,     "Signed 8 bit" },
    { SF_FORMAT_PCM_16,     "Signed 16 bit" },
    { SF_FORMAT_PCM_24,     "Signed 24 bit" },
    { SF_FORMAT_PCM_32,     "Signed 32 bit" },
    { SF_FORMAT_PCM_U8,     "Unsigned 8 bit" },
    { SF_FORMAT_FLOAT,      "32 bit float" },
    { SF_FORMAT_DOUBLE,     "64 bit float" },
    { SF_FORMAT_ULAW,       "U-Law" },
    { SF_FORMAT_ALAW,       "A-Law" },
    { SF_FORMAT_IMA_ADPCM,  "IMA ADPCM" },
    { SF_FORMAT_MS_ADPCM,   "Microsoft ADPCM" },
    { SF_FORMAT_VORBIS,     "Vorbis" },
};
const size_t minorfmt_count = (sizeof(minorfmt_to_string) / sizeof(minorfmt_to_string[0]));

static int fmtcmp(const void * restrict a, const void * restrict b)
{
    return ((struct int_to_str *)a)->key - ((struct int_to_str *)b)->key;
}

static const char *search_int_to_str(const int key, const struct int_to_str *list, size_t listz)
{
    struct int_to_str *n = bsearch(&key, list, listz, sizeof(struct int_to_str), fmtcmp);
    if (n != NULL ) {
        return n->val;
    }

    return NULL;    
}

static inline const char *majorfmt_name(const int format) {
    int major = format & SF_FORMAT_TYPEMASK;
    const char *name = search_int_to_str(major, majorfmt_to_string, majorfmt_count);
    return name ? name : "Unknown container";
}

static inline const char *minorfmt_name(const int format) {
    int minor = format & SF_FORMAT_SUBMASK;
    const char *name = search_int_to_str(minor, minorfmt_to_string, minorfmt_count);
    return name ? name : "Unknown sample format";
}

/* Replaces a file with an HDCD-decoded version.  Optionally keeps the original as
 * <file>.hdcd.
 * Returns -1 on error, 0 if no HDCD was detected, 1 if file was processed.
 */
static int hdcd_replace_file(const char file[restrict 1])
{
    int rv = 0;
    SNDFILE *in = NULL, *out = NULL;
    SF_INFO info_in, info_out;
    int fd_out = -1;
    char tmpout[strlen(file) + 8];
    struct stat instat;

    memset(&info_in, 0, sizeof info_in);
    memset(&info_out, 0, sizeof info_out);

    VERBLOG(2, "%s: scanning for HDCD\n", file);

    errno = 0;
    if (stat(file, &instat) == -1) {
        VERBLOG(0, "%s: unable to stat: %s\n", file, strerror(errno));
        return -1;
    }

    in = sf_open(file, SFM_READ, &info_in);
    if (in == NULL) {
        VERBLOG(0, "%s: unable to open for reading: %s\n", file, sf_strerror(NULL));
        return -1;
    }

    VERBLOG(2, "%s: %s (%s), %d channel%s, %dHz\n", 
            file, majorfmt_name(info_in.format), minorfmt_name(info_in.format), info_in.channels,
            info_in.channels != 1 ? "s" : "", info_in.samplerate);

    /* limit supported input formats to ones that are likely to contain HDCD.
     */
    switch (info_in.format & SF_FORMAT_TYPEMASK) {
        case SF_FORMAT_WAV:
        case SF_FORMAT_AIFF:
        case SF_FORMAT_AU:
        case SF_FORMAT_W64:
        case SF_FORMAT_WAVEX:
        case SF_FORMAT_FLAC:
        case SF_FORMAT_OGG: /* container format, might contain FLAC inside */
        case SF_FORMAT_RF64:
            break;
        default:
            VERBLOG(1, "%s: unsupported container format\n", file);
            rv = 0;
            goto errout_close_input;
    }

    /* HDCD data will only be found in 16 bit, 44k1, stereo data, so we can
     * ignore everything else.
     */
    if ((info_in.samplerate != 44100) ||
        (info_in.channels != 2) ||
        ((info_in.format & SF_FORMAT_SUBMASK) != SF_FORMAT_PCM_16)) {
        VERBLOG(2, "%s: not CD quality, cannot contain HDCD\n", file);
        rv = 0;
        goto errout_close_input;
    }

    if (opt_scan_only == false) {
        snprintf(tmpout, sizeof tmpout, "%s.XXXXXX", file);
        errno = 0;
        fd_out = mkstemp(tmpout);

        if (fd_out == -1) {
            VERBLOG(0, "%s: unable to open %s for writing: %s\n", file, tmpout, strerror(errno));
            rv = -1;
            goto errout_close_input;
        }

        /* attempt to copy across permissions to new file */
        fchmod(fd_out, instat.st_mode);
        fchown(fd_out, instat.st_uid, instat.st_gid);

        memcpy(&info_out, &info_in, sizeof info_out);
        info_out.format = info_in.format & SF_FORMAT_TYPEMASK;
        info_out.format = info_out.format | SF_FORMAT_PCM_24;
        out = sf_open_fd(fd_out, SFM_WRITE, &info_out, false);

        if (out == NULL) {
            VERBLOG(0, "%s: unable to open %s for writing: %s\n", file, tmpout, sf_strerror(NULL));
            rv = -1;
            goto errout_close_output;
        }

        /* if the input was FLAC, set compression level, we might as well set it to
        * maximum and save a little space while we're at it
        */
        if ((info_in.format & SF_FORMAT_TYPEMASK) == SF_FORMAT_FLAC) {
            float clevel = 1.0f;
            sf_command(out, SFC_SET_COMPRESSION_LEVEL, &clevel, sizeof(clevel));
        }

        /* copy any tags that exist */
        for (int idx = SF_STR_FIRST; idx <= SF_STR_LAST; idx++) {
            const char *tag = sf_get_string(in, idx);
            if (tag != NULL) {
                sf_set_string(out, idx, tag);
            }
        }
    }

    int max_samples = 10 * 44100 * 2; /* if no HDCD detected in 10 seconds, skip */
    int lump_size = 10 * 44100 * 2; /* number of samples to process at a time */
    int processed = 0; /* number of samples processsed */
    bool hdcd_found = false; /* have we detected HDCD yet? */

    short *samples_in = calloc(lump_size, sizeof(short)); /* data read via libsndfile */
    int32_t *samples_out = calloc(lump_size, sizeof(int32_t)); /* data processed by libhdcd */

    if (samples_in == NULL || samples_out == NULL) {
        VERBLOG(0, "%s: unable to allocated buffer memory.\n", file);
        rv = -1;
        goto errout_free_buffers;
    }

    hdcd_simple *hdcd = hdcd_new();

    while (true) {
        sf_count_t read = sf_read_short(in, samples_in, lump_size);

        if (read == 0) {
            break;
        }

        /* libhdcd expects 16 bit input to be provided as a 32 bit value with the 
         * sample data in the least significant bits; this is *not* typical when
         * storing PCM sample data in a wider type, where you put the data in the
         * most significant bits and pad down with zeros.
         * upshot: we can't just use sf_read_int()
         */
        for (int sample = 0; sample < read; sample++) {
            samples_out[sample] = (int32_t) samples_in[sample];
        }

        hdcd_process(hdcd, samples_out, read >> 1);
        processed += read;

        if (hdcd_found == false && processed > max_samples) {
            if (hdcd_detected(hdcd) == HDCD_NONE) {
                VERBLOG(2, "%s: no HDCD found in first few seconds, skipping\n", file);
                rv = 0;
                goto errout_free_hdcd;
            }

            VERBLOG(2, "%s: may contain HDCD packets, scanning whole file\n", file);
            hdcd_found = true;
        }

        if (opt_scan_only == false) {
            sf_write_int(out, samples_out, read);
        }
        if (opt_scan_only == true && hdcd_detected(hdcd) == HDCD_EFFECTUAL) {
            VERBLOG(1, "%s: contains at least one effective HDCD packet\n", file);
            rv = 1;
            goto errout_free_hdcd;
        }
    }

    if (opt_scan_only == false && out != NULL) {
        sf_write_sync(out);
    }

    if (hdcd_detected(hdcd) == HDCD_EFFECTUAL) {
        VERBLOG(1, "%s: contained %d effectual HDCD packets, %d samples converted\n", 
                file, hdcd_detect_total_packets(hdcd), processed);
        if (opt_scan_only == false && opt_backup == true) {
            char backname[strlen(file) + 6];
            snprintf(backname, sizeof backname, "%s.hdcd", file);
            errno = 0;
            int renamed = rename(file, backname);
            if (renamed == -1) {
                VERBLOG(0, "%s: unable to backup to %s: %s\n", file, backname, strerror(errno));
                rv = -1;
                goto errout_free_hdcd;
            }
        }
        if (opt_scan_only == false) {
            rename(tmpout, file);
        }
    } else {
        if (hdcd_detect_total_packets(hdcd) > 0) {
            VERBLOG(1, "%s: contained %d HDCD packets to no effect\n", file, hdcd_detect_total_packets(hdcd));
            if (opt_scan_only) {
                /* it's probably not worth reporting this has having HDCD as
                 * there is nothing to do be done
                 */
                rv = 0;
                goto errout_free_hdcd;            
            }
        } else {
            VERBLOG(2, "%s: no HDCD found\n", file);
        }
    }

    rv = !!hdcd_found;

errout_free_hdcd:
    hdcd_free(hdcd);

errout_free_buffers:
    free(samples_out);
    free(samples_in);

errout_close_output:
    if (opt_scan_only == false) {
        sf_close(out);
        close(fd_out);
        remove(tmpout);
    }

errout_close_input:
    sf_close(in);
    return rv;
}

static void print_usage(FILE * restrict where, const char * restrict what)
{
    char sndfilever[64];
    int hdcdmajor, hdcdminor;

    sf_command(NULL, SFC_GET_LIB_VERSION, sndfilever, sizeof sndfilever);
    hdcd_lib_version(&hdcdmajor, &hdcdminor);

    fprintf(where, "hdcp-fixup %d.%d: detect and remove HDCD encoding from audio files\n",
            VER_MAJOR, VER_MINOR);
    fprintf(where, "libraries in use: %s libhdcd-%d.%d\n\n", sndfilever, hdcdmajor, hdcdminor);
    fprintf(where, "usage: %s [options] file [fileN...]\n", what);
    fprintf(where, "\t-v\tverbose operation\n");
    fprintf(where, "\t-vv\tvery verbose operation\n");
    fprintf(where, "\t-b\tdo NOT create a .hdcd backup of original when replacing\n");
    fprintf(where, "\t-s\tscan and report only, returns 1 if no HDCD, 0 otherwise\n");
}

int main(int argc, char *argv[])
{
    const char *opts = "hvbs";
    int opt;

    while ((opt = getopt(argc, argv, opts)) != -1) {
        switch (opt) {
        case 'h':
            print_usage(stdout, argv[0]);
            exit(EXIT_SUCCESS);
            break;
        case 'v':
            opt_verbose++;
            break;
        case 'b':
            opt_backup = false;
            break;
        case 's':
            opt_scan_only = true;
            break;
        default:
            print_usage(stderr, argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    if (opt_scan_only == true) {
        if (argc - optind != 1) {
            fprintf(stderr, "when using -s, precisely one file must be given.\n");
            exit(EXIT_FAILURE);
        }

        int r = hdcd_replace_file(argv[optind]);
        switch (r) {
        case -1:
            exit(EXIT_FAILURE);
            break;
        case 0:
            exit(1);
            break;
        case 1:
            exit(0);
            break;
        }
        exit(EXIT_FAILURE); /* shouldn't get here */
    }

    for (int i = optind; i < argc; i++) {
        int r = hdcd_replace_file(argv[i]);
        if (r == -1) {
            exit(EXIT_FAILURE);
        }
    }
}

