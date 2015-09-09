/**
 * hdr_histogram_log.c
 * Written by Michael Barker and released to the public domain,
 * as explained at http://creativecommons.org/publicdomain/zero/1.0/
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>
#include <errno.h>
#include <ctype.h>
#include <math.h>
#include <time.h>

#include "hdr_encoding.h"
#include "hdr_histogram.h"
#include "hdr_histogram_log.h"

#ifdef __APPLE__

#include <libkern/OSByteOrder.h>

#define htobe16(x) OSSwapHostToBigInt16(x)
#define htole16(x) OSSwapHostToLittleInt16(x)
#define be16toh(x) OSSwapBigToHostInt16(x)
#define le16toh(x) OSSwapLittleToHostInt16(x)

#define htobe32(x) OSSwapHostToBigInt32(x)
#define htole32(x) OSSwapHostToLittleInt32(x)
#define be32toh(x) OSSwapBigToHostInt32(x)
#define le32toh(x) OSSwapLittleToHostInt32(x)

#define htobe64(x) OSSwapHostToBigInt64(x)
#define htole64(x) OSSwapHostToLittleInt64(x)
#define be64toh(x) OSSwapBigToHostInt64(x)
#define le64toh(x) OSSwapLittleToHostInt64(x)

#endif

// Private prototypes useful for the logger
int32_t counts_index_for(const struct hdr_histogram* h, int64_t value);


#define FAIL_AND_CLEANUP(label, error_name, error) \
    do                      \
    {                       \
        error_name = error; \
        goto label;         \
    }                       \
    while (0)

enum zero_strategy { ZERO_ALL, ZERO_NONE };

int realloc_buffer(
    void** buffer, size_t nmemb, ssize_t size, enum zero_strategy zeroing)
{
    size_t len = nmemb * size;
    if (NULL == *buffer)
    {
        *buffer = malloc(len);
    }
    else
    {
        *buffer = realloc(*buffer, len);
    }

    if (NULL == *buffer)
    {
        return ENOMEM;
    }
    else
    {
        if (zeroing == ZERO_ALL)
        {
            memset(*buffer, 0, len);
        }
        return 0;
    }
}

//  ######  ######## ########  #### ##    ##  ######    ######
// ##    ##    ##    ##     ##  ##  ###   ## ##    ##  ##    ##
// ##          ##    ##     ##  ##  ####  ## ##        ##
//  ######     ##    ########   ##  ## ## ## ##   ####  ######
//       ##    ##    ##   ##    ##  ##  #### ##    ##        ##
// ##    ##    ##    ##    ##   ##  ##   ### ##    ##  ##    ##
//  ######     ##    ##     ## #### ##    ##  ######    ######

ssize_t null_trailing_whitespace(char* s, ssize_t len)
{
    ssize_t i = len;
    while (--i != -1)
    {
        if (isspace(s[i]))
        {
            s[i] = '\0';
        }
        else
        {
            return i + 1;
        }
    }

    return 0;
}

// ######## ##    ##  ######   #######  ########  #### ##    ##  ######
// ##       ###   ## ##    ## ##     ## ##     ##  ##  ###   ## ##    ##
// ##       ####  ## ##       ##     ## ##     ##  ##  ####  ## ##
// ######   ## ## ## ##       ##     ## ##     ##  ##  ## ## ## ##   ####
// ##       ##  #### ##       ##     ## ##     ##  ##  ##  #### ##    ##
// ##       ##   ### ##    ## ##     ## ##     ##  ##  ##   ### ##    ##
// ######## ##    ##  ######   #######  ########  #### ##    ##  ######

static const int32_t V0_ENCODING_COOKIE    = 0x1c849308;
static const int32_t V0_COMPRESSION_COOKIE = 0x1c849309;

static const int32_t V1_ENCODING_COOKIE    = 0x1c849301;
static const int32_t V1_COMPRESSION_COOKIE = 0x1c849302;

static const int32_t V2_ENCODING_COOKIE = 0x1c849303;
static const int32_t V2_COMPRESSION_COOKIE = 0x1c849304;

int32_t get_cookie_base(int32_t cookie)
{
    return (cookie & ~0xf0);
}

int32_t word_size_from_cookie(int32_t cookie)
{
    return (cookie & 0xf0) >> 4;
}

const char* hdr_strerror(int errnum)
{
    switch (errnum)
    {
        case HDR_COMPRESSION_COOKIE_MISMATCH:
            return "Compression cookie mismatch";
        case HDR_ENCODING_COOKIE_MISMATCH:
            return "Encoding cookie mismatch";
        case HDR_DEFLATE_INIT_FAIL:
            return "Deflate initialisation failed";
        case HDR_DEFLATE_FAIL:
            return "Deflate failed";
        case HDR_INFLATE_INIT_FAIL:
            return "Inflate initialisation failed";
        case HDR_INFLATE_FAIL:
            return "Inflate failed";
        case HDR_LOG_INVALID_VERSION:
            return "Log - invalid version in log header";
        default:
            return strerror(errnum);
    }
}

static void strm_init(z_stream* strm)
{
    strm->zfree = NULL;
    strm->zalloc = NULL;
    strm->opaque = NULL;
    strm->next_in = NULL;
    strm->avail_in = 0;
}

union uint64_dbl_cvt
{
    uint64_t l;
    double d;
};

static double int64_bits_to_double(int64_t i)
{
    union uint64_dbl_cvt x;
    
    x.l = (uint64_t) i;
    return x.d;
}

static uint64_t double_to_int64_bits(double d)
{
    union uint64_dbl_cvt x;

    x.d = d;
    return x.l;
}

typedef struct __attribute__((__packed__))
{
    int32_t cookie;
    int32_t significant_figures;
    int64_t lowest_trackable_value;
    int64_t highest_trackable_value;
    int64_t total_count;
    int64_t counts[0];
} _encoding_flyweight_v0;

typedef struct __attribute__((__packed__))
{
    int32_t cookie;
    int32_t payload_len;
    int32_t normalizing_index_offset;
    int32_t significant_figures;
    int64_t lowest_trackable_value;
    int64_t highest_trackable_value;
    uint64_t conversion_ratio_bits;
    int64_t counts[0];
} _encoding_flyweight_v1;

typedef struct __attribute__((__packed__))
{
    int32_t cookie;
    int32_t length;
    uint8_t data[0];
} _compression_flyweight;

int hdr_encode_compressed(
    struct hdr_histogram* h,
    uint8_t** compressed_histogram,
    size_t* compressed_len)
{
    _encoding_flyweight_v1* encoded = NULL;
    _compression_flyweight* compressed = NULL;
    int result = 0;

    int32_t len_to_max = counts_index_for(h, h->max_value) + 1;
    int32_t counts_limit = len_to_max < h->counts_len ? len_to_max : h->counts_len;

    size_t encoded_size =
        sizeof(_encoding_flyweight_v1) + (sizeof(int64_t) * counts_limit);

    if ((encoded = (_encoding_flyweight_v1*) malloc(encoded_size)) == NULL)
    {
        FAIL_AND_CLEANUP(cleanup, result, ENOMEM);
    }

    // Estimate the size of the compressed histogram.
    uLongf destLen = compressBound(encoded_size);
    size_t compressed_size = sizeof(_compression_flyweight) + destLen;

    if ((compressed = (_compression_flyweight*) malloc(compressed_size)) == NULL)
    {
        FAIL_AND_CLEANUP(cleanup, result, ENOMEM);
    }

    encoded->cookie                   = htobe32(V1_ENCODING_COOKIE + (8 << 4));
    encoded->payload_len              = htobe32(encoded_size);
    encoded->normalizing_index_offset = htobe32(h->normalizing_index_offset);
    encoded->significant_figures      = htobe32(h->significant_figures);
    encoded->lowest_trackable_value   = htobe64(h->lowest_trackable_value);
    encoded->highest_trackable_value  = htobe64(h->highest_trackable_value);
    encoded->conversion_ratio_bits    = htobe64(double_to_int64_bits(h->conversion_ratio));

    for (int i = 0; i < counts_limit; i++)
    {
        encoded->counts[i] = htobe64(h->counts[i]);
    }

    if (Z_OK != compress(compressed->data, &destLen, (Bytef*) encoded, encoded_size))
    {
        FAIL_AND_CLEANUP(cleanup, result, HDR_DEFLATE_FAIL);
    }

    compressed->cookie = htobe32(V1_COMPRESSION_COOKIE + (8 << 4));
    compressed->length = htobe32((int32_t)destLen);

    *compressed_histogram = (uint8_t*) compressed;
    *compressed_len = sizeof(_compression_flyweight) + destLen;

    cleanup:
    free(encoded);
    if (result == HDR_DEFLATE_FAIL)
    {
        free(compressed);
    }

    return result;
}

// ########  ########  ######   #######  ########  #### ##    ##  ######
// ##     ## ##       ##    ## ##     ## ##     ##  ##  ###   ## ##    ##
// ##     ## ##       ##       ##     ## ##     ##  ##  ####  ## ##
// ##     ## ######   ##       ##     ## ##     ##  ##  ## ## ## ##   ####
// ##     ## ##       ##       ##     ## ##     ##  ##  ##  #### ##    ##
// ##     ## ##       ##    ## ##     ## ##     ##  ##  ##   ### ##    ##
// ########  ########  ######   #######  ########  #### ##    ##  ######

// Prototype to avoid exposing in header
void hdr_reset_internal_counters(struct hdr_histogram* h);

static void _apply_to_counts_16(struct hdr_histogram* h, const int16_t* counts_data, const int32_t counts_limit)
{
    for (int i = 0; i < counts_limit; i++)
    {
        h->counts[i] = be16toh(counts_data[i]);
    }
}

static void _apply_to_counts_32(struct hdr_histogram* h, const int32_t* counts_data, const int32_t counts_limit)
{
    for (int i = 0; i < counts_limit; i++)
    {
        h->counts[i] = be32toh(counts_data[i]);
    }
}

static void _apply_to_counts_64(struct hdr_histogram* h, const int64_t* counts_data, const int32_t counts_limit)
{
    for (int i = 0; i < counts_limit; i++)
    {
        h->counts[i] = be64toh(counts_data[i]);
    }
}

static int _apply_to_counts_zz(struct hdr_histogram* h, const uint8_t* counts_data, const int32_t data_limit)
{
    int32_t data_index = 0;
    int32_t counts_index = 0;
    int64_t value;

    while (data_index < data_limit)
    {
        data_index += zig_zag_decode_i64(&counts_data[data_index], &value);

        int64_t host_value = (int64_t) le64toh(value);
        if (host_value < INT32_MIN)
        {
            return HDR_TRAILING_ZEROS_INVALID;
        }

        int32_t trailing_zeros = 0;
        if (host_value < 0)
        {
            trailing_zeros = (int32_t) host_value;

            data_index += zig_zag_decode_i64(&counts_data[data_index], &value);
            host_value = (int64_t) le64toh(value);
        }

        h->counts[counts_index] = host_value;
        counts_index++;
        counts_index += (-trailing_zeros);
    }

    return 0;
}

static int _apply_to_counts(
    struct hdr_histogram* h, const int32_t word_size, const uint8_t* counts_data, const int32_t counts_limit)
{
    switch (word_size)
    {
        case 2:
            _apply_to_counts_16(h, (int16_t*) counts_data, counts_limit);
            return 0;

        case 4:
            _apply_to_counts_32(h, (int32_t*) counts_data, counts_limit);
            return 0;

        case 8:
            _apply_to_counts_64(h, (int64_t*) counts_data, counts_limit);
            return 0;

        case 1:
            return _apply_to_counts_zz(h, counts_data, counts_limit);

        default:
            return -1;
    }
}

static int hdr_decode_compressed_v0(
    _compression_flyweight* compression_flyweight,
    size_t length,
    struct hdr_histogram** histogram)
{
    struct hdr_histogram* h = NULL;
    int result = 0;
    uint8_t* counts_array = NULL;
    _encoding_flyweight_v0 encoding_flyweight;
    z_stream strm;

    strm_init(&strm);
    if (inflateInit(&strm) != Z_OK)
    {
        FAIL_AND_CLEANUP(cleanup, result, HDR_INFLATE_FAIL);
    }

    int32_t compressed_length = be32toh(compression_flyweight->length);

    if (compressed_length < 0 || length - sizeof(_compression_flyweight) < compressed_length)
    {
        FAIL_AND_CLEANUP(cleanup, result, EINVAL);
    }

    strm.next_in = compression_flyweight->data;
    strm.avail_in = (uInt) compressed_length;
    strm.next_out = (uint8_t *) &encoding_flyweight;
    strm.avail_out = sizeof(_encoding_flyweight_v0);

    if (inflate(&strm, Z_SYNC_FLUSH) != Z_OK)
    {
        FAIL_AND_CLEANUP(cleanup, result, HDR_INFLATE_FAIL);
    }

    int32_t encoding_cookie = get_cookie_base(be32toh(encoding_flyweight.cookie));
    if (V0_ENCODING_COOKIE != encoding_cookie)
    {
        FAIL_AND_CLEANUP(cleanup, result, HDR_ENCODING_COOKIE_MISMATCH);
    }

    int32_t word_size = word_size_from_cookie(be32toh(encoding_flyweight.cookie));
    int64_t lowest_trackable_value = be64toh(encoding_flyweight.lowest_trackable_value);
    int64_t highest_trackable_value = be64toh(encoding_flyweight.highest_trackable_value);
    int32_t significant_figures = be32toh(encoding_flyweight.significant_figures);

    if (hdr_init(
        lowest_trackable_value,
        highest_trackable_value,
        significant_figures,
        &h) != 0)
    {
        FAIL_AND_CLEANUP(cleanup, result, ENOMEM);
    }

    int32_t counts_array_len = h->counts_len * word_size;
    if ((counts_array = calloc(1, (size_t) counts_array_len)) == NULL)
    {
        FAIL_AND_CLEANUP(cleanup, result, ENOMEM);
    }

    strm.next_out = counts_array;
    strm.avail_out = (uInt) counts_array_len;

    if (inflate(&strm, Z_FINISH) != Z_STREAM_END)
    {
        FAIL_AND_CLEANUP(cleanup, result, HDR_INFLATE_FAIL);
    }

    _apply_to_counts(h, word_size, counts_array, h->counts_len);

    hdr_reset_internal_counters(h);
    h->normalizing_index_offset = 0;
    h->conversion_ratio = 1.0;

cleanup:
    (void)inflateEnd(&strm);
    free(counts_array);

    if (result != 0)
    {
        free(h);
    }
    else if (NULL == *histogram)
    {
        *histogram = h;
    }
    else
    {
        hdr_add(*histogram, h);
        free(h);
    }

    return result;
}

static int hdr_decode_compressed_v1(
    _compression_flyweight* compression_flyweight,
    size_t length,
    struct hdr_histogram** histogram)
{
    struct hdr_histogram* h = NULL;
    int result = 0;
    uint8_t* counts_array = NULL;
    _encoding_flyweight_v1 encoding_flyweight;
    z_stream strm;

    strm_init(&strm);
    if (inflateInit(&strm) != Z_OK)
    {
        FAIL_AND_CLEANUP(cleanup, result, HDR_INFLATE_FAIL);
    }

    int32_t compressed_length = be32toh(compression_flyweight->length);

    if (compressed_length < 0 || length - sizeof(_compression_flyweight) < compressed_length)
    {
        FAIL_AND_CLEANUP(cleanup, result, EINVAL);
    }

    strm.next_in = compression_flyweight->data;
    strm.avail_in = (uInt) compressed_length;
    strm.next_out = (uint8_t *) &encoding_flyweight;
    strm.avail_out = sizeof(_encoding_flyweight_v1);

    if (inflate(&strm, Z_SYNC_FLUSH) != Z_OK)
    {
        FAIL_AND_CLEANUP(cleanup, result, HDR_INFLATE_FAIL);
    }

    int32_t encoding_cookie = get_cookie_base(be32toh(encoding_flyweight.cookie));
    if (V1_ENCODING_COOKIE != encoding_cookie)
    {
        FAIL_AND_CLEANUP(cleanup, result, HDR_ENCODING_COOKIE_MISMATCH);
    }

    int32_t word_size = word_size_from_cookie(be32toh(encoding_flyweight.cookie));
    int32_t counts_limit = be32toh(encoding_flyweight.payload_len) / word_size;
    int64_t lowest_trackable_value = be64toh(encoding_flyweight.lowest_trackable_value);
    int64_t highest_trackable_value = be64toh(encoding_flyweight.highest_trackable_value);
    int32_t significant_figures = be32toh(encoding_flyweight.significant_figures);

    if (hdr_init(
        lowest_trackable_value,
        highest_trackable_value,
        significant_figures,
        &h) != 0)
    {
        FAIL_AND_CLEANUP(cleanup, result, ENOMEM);
    }

    // Give the temp uncompressed array a little bif of extra
    int32_t counts_array_len = counts_limit * word_size;

    if ((counts_array = calloc(1, (size_t) counts_array_len)) == NULL)
    {
        FAIL_AND_CLEANUP(cleanup, result, ENOMEM);
    }

    strm.next_out = counts_array;
    strm.avail_out = (uInt) counts_array_len;

    if (inflate(&strm, Z_FINISH) != Z_STREAM_END)
    {
        FAIL_AND_CLEANUP(cleanup, result, HDR_INFLATE_FAIL);
    }

    _apply_to_counts(h, word_size, counts_array, counts_limit);

    h->normalizing_index_offset = be32toh(encoding_flyweight.normalizing_index_offset);
    h->conversion_ratio = int64_bits_to_double(be64toh(encoding_flyweight.conversion_ratio_bits));
    hdr_reset_internal_counters(h);

cleanup:
    (void)inflateEnd(&strm);
    free(counts_array);

    if (result != 0)
    {
        free(h);
    }
    else if (NULL == *histogram)
    {
        *histogram = h;
    }
    else
    {
        hdr_add(*histogram, h);
        free(h);
    }

    return result;
}

static int hdr_decode_compressed_v2(
    _compression_flyweight* compression_flyweight,
    size_t length,
    struct hdr_histogram** histogram)
{
    struct hdr_histogram* h = NULL;
    int result = 0;
    uint8_t* counts_array = NULL;
    _encoding_flyweight_v1 encoding_flyweight;
    z_stream strm;

    strm_init(&strm);
    if (inflateInit(&strm) != Z_OK)
    {
        FAIL_AND_CLEANUP(cleanup, result, HDR_INFLATE_FAIL);
    }

    int32_t compressed_length = be32toh(compression_flyweight->length);

    if (compressed_length < 0 || length - sizeof(_compression_flyweight) < compressed_length)
    {
        FAIL_AND_CLEANUP(cleanup, result, EINVAL);
    }

    strm.next_in = compression_flyweight->data;
    strm.avail_in = (uInt) compressed_length;
    strm.next_out = (uint8_t *) &encoding_flyweight;
    strm.avail_out = sizeof(_encoding_flyweight_v1);

    if (inflate(&strm, Z_SYNC_FLUSH) != Z_OK)
    {
        FAIL_AND_CLEANUP(cleanup, result, HDR_INFLATE_FAIL);
    }

    int32_t encoding_cookie = get_cookie_base(be32toh(encoding_flyweight.cookie));
    if (V2_ENCODING_COOKIE != encoding_cookie)
    {
        FAIL_AND_CLEANUP(cleanup, result, HDR_ENCODING_COOKIE_MISMATCH);
    }

    int32_t word_size = 1;
    int32_t counts_limit = be32toh(encoding_flyweight.payload_len);
    int64_t lowest_trackable_value = be64toh(encoding_flyweight.lowest_trackable_value);
    int64_t highest_trackable_value = be64toh(encoding_flyweight.highest_trackable_value);
    int32_t significant_figures = be32toh(encoding_flyweight.significant_figures);

    if (hdr_init(
        lowest_trackable_value,
        highest_trackable_value,
        significant_figures,
        &h) != 0)
    {
        FAIL_AND_CLEANUP(cleanup, result, ENOMEM);
    }

    // Give the temp uncompressed array a little bif of extra
    int32_t counts_array_len = counts_limit * word_size;

    if ((counts_array = calloc(1, (size_t) counts_array_len)) == NULL)
    {
        FAIL_AND_CLEANUP(cleanup, result, ENOMEM);
    }

    strm.next_out = counts_array;
    strm.avail_out = (uInt) counts_array_len;

    if (inflate(&strm, Z_FINISH) != Z_STREAM_END)
    {
        FAIL_AND_CLEANUP(cleanup, result, HDR_INFLATE_FAIL);
    }

    int r =_apply_to_counts(h, word_size, counts_array, counts_limit);
    if (0 != r)
    {
        FAIL_AND_CLEANUP(cleanup, result, r);
    }

    h->normalizing_index_offset = be32toh(encoding_flyweight.normalizing_index_offset);
    h->conversion_ratio = int64_bits_to_double(be64toh(encoding_flyweight.conversion_ratio_bits));
    hdr_reset_internal_counters(h);

cleanup:
    (void)inflateEnd(&strm);
    free(counts_array);

    if (result != 0)
    {
        free(h);
    }
    else if (NULL == *histogram)
    {
        *histogram = h;
    }
    else
    {
        hdr_add(*histogram, h);
        free(h);
    }

    return result;
}

int hdr_decode_compressed(
    uint8_t* buffer, size_t length, struct hdr_histogram** histogram)
{
    if (length < sizeof(_compression_flyweight))
    {
        return EINVAL;
    }

    _compression_flyweight* compression_flyweight = (_compression_flyweight*) buffer;

    int32_t compression_cookie = get_cookie_base(be32toh(compression_flyweight->cookie));
    if (V0_COMPRESSION_COOKIE == compression_cookie)
    {
        return hdr_decode_compressed_v0(compression_flyweight, length, histogram);
    }
    else if (V1_COMPRESSION_COOKIE == compression_cookie)
    {
        return hdr_decode_compressed_v1(compression_flyweight, length, histogram);
    }
    else if (V2_COMPRESSION_COOKIE == compression_cookie)
    {
        return hdr_decode_compressed_v2(compression_flyweight, length, histogram);
    }

    return HDR_COMPRESSION_COOKIE_MISMATCH;
}

// ##      ## ########  #### ######## ######## ########
// ##  ##  ## ##     ##  ##     ##    ##       ##     ##
// ##  ##  ## ##     ##  ##     ##    ##       ##     ##
// ##  ##  ## ########   ##     ##    ######   ########
// ##  ##  ## ##   ##    ##     ##    ##       ##   ##
// ##  ##  ## ##    ##   ##     ##    ##       ##    ##
//  ###  ###  ##     ## ####    ##    ######## ##     ##

int hdr_log_writer_init(struct hdr_log_writer* writer)
{
    return 0;
}

#define LOG_VERSION "1.1"
#define LOG_MAJOR_VERSION 1
#define LOG_MINOR_VERSION 1

static int print_user_prefix(FILE* f, const char* prefix)
{
    if (!prefix)
    {
        return 0;
    }

    return fprintf(f, "#[%s]\n", prefix);
}

static int print_version(FILE* f, const char* version)
{
    return fprintf(f, "#[Histogram log format version %s]\n", version);
}

static int print_time(FILE* f, struct timespec* timestamp)
{
    char time_str[128];
    struct tm date_time;

    if (!timestamp)
    {
        return 0;
    }

    gmtime_r(&timestamp->tv_sec, &date_time);
    long ms = timestamp->tv_nsec / 1000000;
    strftime(time_str, 128, "%a %b %X %Z %Y", &date_time);

    return fprintf(
        f, "#[StartTime: %d.%ld (seconds since epoch), %s]\n",
        (int) timestamp->tv_sec, ms, time_str);
}

static int print_header(FILE* f)
{
    return fprintf(f, "\"StartTimestamp\",\"EndTimestamp\",\"Interval_Max\",\"Interval_Compressed_Histogram\"\n");
}

// Example log
// #[Logged with jHiccup version 2.0.3-SNAPSHOT]
// #[Histogram log format version 1.01]
// #[StartTime: 1403476110.183 (seconds since epoch), Mon Jun 23 10:28:30 NZST 2014]
// "StartTimestamp","EndTimestamp","Interval_Max","Interval_Compressed_Histogram"
int hdr_log_write_header(
    struct hdr_log_writer* writer, FILE* file,
    const char* user_prefix, struct timespec* timestamp)
{
    if (print_user_prefix(file, user_prefix) < 0)
    {
        return EIO;
    }
    if (print_version(file, LOG_VERSION) < 0)
    {
        return EIO;
    }
    if (print_time(file, timestamp) < 0)
    {
        return EIO;
    }
    if (print_header(file) < 0)
    {
        return EIO;
    }

    return 0;
}

int hdr_log_write(
    struct hdr_log_writer* writer,
    FILE* file,
    const struct timespec* start_timestamp,
    const struct timespec* end_timestamp,
    struct hdr_histogram* histogram)
{
    uint8_t* compressed_histogram = NULL;
    size_t compressed_len = 0;
    char* encoded_histogram = NULL;
    int rc = 0;
    int result = 0;
    size_t encoded_len;

    rc = hdr_encode_compressed(histogram, &compressed_histogram, &compressed_len);
    if (rc != 0)
    {
        FAIL_AND_CLEANUP(cleanup, result, rc);
    }

    encoded_len = base64_encoded_len(compressed_len);
    encoded_histogram = calloc(encoded_len + 1, sizeof(char));

    rc = base64_encode(
        compressed_histogram, compressed_len, encoded_histogram, encoded_len);
    if (rc != 0)
    {
        FAIL_AND_CLEANUP(cleanup, result, rc);
    }

    if (fprintf(
        file, "%d.%d,%d.%d,%"PRIu64".0,%s\n",
        (int) start_timestamp->tv_sec, (int) (start_timestamp->tv_nsec / 1000000),
        (int) end_timestamp->tv_sec, (int) (end_timestamp->tv_nsec / 1000000),
        hdr_max(histogram),
        encoded_histogram) < 0)
    {
        result = EIO;
    }

cleanup:
    free(compressed_histogram);
    free(encoded_histogram);

    return result;
}

// ########  ########    ###    ########  ######## ########
// ##     ## ##         ## ##   ##     ## ##       ##     ##
// ##     ## ##        ##   ##  ##     ## ##       ##     ##
// ########  ######   ##     ## ##     ## ######   ########
// ##   ##   ##       ######### ##     ## ##       ##   ##
// ##    ##  ##       ##     ## ##     ## ##       ##    ##
// ##     ## ######## ##     ## ########  ######## ##     ##

int hdr_log_reader_init(struct hdr_log_reader* reader)
{
    reader->major_version = 0;
    reader->minor_version = 0;
    reader->start_timestamp.tv_sec = 0;
    reader->start_timestamp.tv_nsec = 0;

    return 0;
}

static void scan_log_format(struct hdr_log_reader* reader, const char* line)
{
    const char* format = "#[Histogram log format version %d.%d]";
    sscanf(line, format, &reader->major_version, &reader->minor_version);
}

static void scan_start_time(struct hdr_log_reader* reader, const char* line)
{
    const char* format = "#[StartTime: %d.%d [^\n]";
    int timestamp_s = 0;
    int trailing_ms = 0;

    if (sscanf(line, format, &timestamp_s, &trailing_ms) == 2)
    {
        reader->start_timestamp.tv_sec = timestamp_s;
        reader->start_timestamp.tv_nsec = trailing_ms * 1000000;
    }
}

static void scan_header_line(struct hdr_log_reader* reader, const char* line)
{
    scan_log_format(reader, line);
    scan_start_time(reader, line);
}

static bool validate_log_version(struct hdr_log_reader* reader)
{
    return reader->major_version == LOG_MAJOR_VERSION &&
        (reader->minor_version == 0 || reader->minor_version == 1 || reader->minor_version == 2);
}

#define HEADER_LINE_LENGTH 128

int hdr_log_read_header(struct hdr_log_reader* reader, FILE* file)
{
    char line[HEADER_LINE_LENGTH]; // TODO: check for overflow.

    bool parsing_header = true;

    do
    {
        int c = fgetc(file);
        ungetc(c, file);

        switch (c)
        {

        case '#':
            if (fgets(line, HEADER_LINE_LENGTH, file) == NULL)
            {
                return EIO;
            }

            scan_header_line(reader, line);
            break;

        case '"':
            if (fgets(line, HEADER_LINE_LENGTH, file) == NULL)
            {
                return EIO;
            }

            parsing_header = false;
            break;

        default:
            parsing_header = false;
        }
    }
    while (parsing_header);

    if (!validate_log_version(reader))
    {
        return HDR_LOG_INVALID_VERSION;
    }

    return 0;
}

static void update_timespec(struct timespec* ts, int time_s, int time_ms)
{
    if (NULL == ts)
    {
        return;
    }

    ts->tv_sec = time_s;
    ts->tv_nsec = time_ms * 1000000;
}

int hdr_log_read(
    struct hdr_log_reader* reader, FILE* file, struct hdr_histogram** histogram,
    struct timespec* timestamp, struct timespec* interval)
{
    const char* format = "%d.%d,%d.%d,%d.%d,%s";
    char* base64_histogram = NULL;
    uint8_t* compressed_histogram = NULL;
    char* line = NULL;
    size_t line_len = 0;
    int result = 0;

    int begin_s = 0;
    int begin_ms = 0;
    int end_s = 0;
    int end_ms = 0;
    int interval_max_s = 0;
    int interval_max_ms = 0;

    ssize_t read = getline(&line, &line_len, file);
    if (-1 == read)
    {
        if (0 == errno)
        {
            FAIL_AND_CLEANUP(cleanup, result, EOF);
        }
        else
        {
            FAIL_AND_CLEANUP(cleanup, result, EIO);
        }
    }

    null_trailing_whitespace(line, read);
    if (strlen(line) == 0)
    {
        FAIL_AND_CLEANUP(cleanup, result, EOF);
    }

    int r;
    r = realloc_buffer(
        (void**)&base64_histogram, sizeof(char), read, ZERO_ALL);
    if (r != 0)
    {
        FAIL_AND_CLEANUP(cleanup, result, ENOMEM);
    }

    r = realloc_buffer(
        (void**)&compressed_histogram, sizeof(uint8_t), read, ZERO_ALL);
    if (r != 0)
    {
        FAIL_AND_CLEANUP(cleanup, result, ENOMEM);
    }

    int num_tokens = sscanf(
        line, format, &begin_s, &begin_ms, &end_s, &end_ms,
        &interval_max_s, &interval_max_ms, base64_histogram);

    if (num_tokens != 7)
    {
        FAIL_AND_CLEANUP(cleanup, result, EINVAL);
    }

    size_t base64_len = strlen(base64_histogram);
    size_t compressed_len = base64_decoded_len(base64_len);

    r = base64_decode(
        base64_histogram, base64_len, compressed_histogram, compressed_len);

    if (r != 0)
    {
        FAIL_AND_CLEANUP(cleanup, result, r);
    }

    r = hdr_decode_compressed(compressed_histogram, compressed_len, histogram);
    if (r != 0)
    {
        FAIL_AND_CLEANUP(cleanup, result, r);
    }

    update_timespec(timestamp, begin_s, begin_ms);
    update_timespec(interval, end_s, end_ms);

cleanup:
    free(line);
    free(base64_histogram);
    free(compressed_histogram);

    return result;
}

int hdr_log_encode(struct hdr_histogram* histogram, char** encoded_histogram)
{
    char *encoded_histogram_tmp = NULL;
    uint8_t* compressed_histogram = NULL;
    size_t compressed_len = 0;
    int rc = 0;
    int result = 0;
    size_t encoded_len;

    rc = hdr_encode_compressed(histogram, &compressed_histogram, &compressed_len);
    if (rc != 0)
    {
        FAIL_AND_CLEANUP(cleanup, result, rc);
    }

    encoded_len = base64_encoded_len(compressed_len);
    encoded_histogram_tmp = calloc(encoded_len + 1, sizeof(char));

    rc = base64_encode(
        compressed_histogram, compressed_len, encoded_histogram_tmp, encoded_len);
    if (rc != 0)
    {
        FAIL_AND_CLEANUP(cleanup, result, rc);
    }

    *encoded_histogram = encoded_histogram_tmp;

cleanup:
    free(compressed_histogram);

    return result;
}

int hdr_log_decode(struct hdr_histogram** histogram, char* base64_histogram, size_t base64_len)
{
    int r;
    uint8_t* compressed_histogram = NULL;
    int result = 0;

    size_t compressed_len = base64_decoded_len(base64_len);
    compressed_histogram = malloc(sizeof(uint8_t)*compressed_len);
    memset(compressed_histogram, 0, compressed_len);

    r = base64_decode(
        base64_histogram, base64_len, compressed_histogram, compressed_len);

    if (r != 0)
    {
        FAIL_AND_CLEANUP(cleanup, result, r);
    }

    r = hdr_decode_compressed(compressed_histogram, compressed_len, histogram);
    if (r != 0)
    {
        FAIL_AND_CLEANUP(cleanup, result, r);
    }

cleanup:
    free(compressed_histogram);

    return result;
}
