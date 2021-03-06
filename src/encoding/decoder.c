#include <string.h>
#include "decoder.h"
#include "convert.h"
#include "../editor.h"
#include "../util/hashset.h"
#include "../util/utf8.h"
#include "../util/xmalloc.h"

static bool fill(FileDecoder *dec)
{
    size_t icount = dec->isize - dec->ipos;

    // Smaller than cconv.obuf to make realloc less likely
    size_t max = 7 * 1024;

    if (icount > max) {
        icount = max;
    }

    if (dec->ipos == dec->isize) {
        return false;
    }

    cconv_process(dec->cconv, dec->ibuf + dec->ipos, icount);
    dec->ipos += icount;
    if (dec->ipos == dec->isize) {
        // Must be flushed after all input has been fed
        cconv_flush(dec->cconv);
    }
    return true;
}

static bool decode_and_read_line(FileDecoder *dec, char **linep, size_t *lenp)
{
    char *line;
    size_t len;

    while (1) {
        line = cconv_consume_line(dec->cconv, &len);
        if (line) {
            break;
        }

        if (!fill(dec)) {
            break;
        }
    }

    if (line) {
        // Newline not wanted
        len--;
    } else {
        line = cconv_consume_all(dec->cconv, &len);
        if (len == 0) {
            return false;
        }
    }

    *linep = line;
    *lenp = len;
    return true;
}

static bool read_utf8_line(FileDecoder *dec, char **linep, size_t *lenp)
{
    char *line = (char *)dec->ibuf + dec->ipos;
    const char *nl = memchr(line, '\n', dec->isize - dec->ipos);
    size_t len;

    if (nl) {
        len = nl - line;
        dec->ipos += len + 1;
    } else {
        len = dec->isize - dec->ipos;
        if (len == 0) {
            return false;
        }
        dec->ipos += len;
    }

    *linep = line;
    *lenp = len;
    return true;
}

static int set_encoding(FileDecoder *dec, const char *encoding)
{
    if (strcmp(encoding, "UTF-8") == 0) {
        dec->read_line = read_utf8_line;
    } else {
        dec->cconv = cconv_to_utf8(encoding);
        if (dec->cconv == NULL) {
            return -1;
        }
        dec->read_line = decode_and_read_line;
    }
    dec->encoding = str_intern(encoding);
    return 0;
}

static bool detect(FileDecoder *dec, const unsigned char *line, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (line[i] >= 0x80) {
            size_t idx = i;
            CodePoint u = u_get_nonascii(line, len, &idx);
            const char *encoding;
            if (u_is_unicode(u)) {
                encoding = "UTF-8";
            } else if (editor.term_utf8) {
                if (dec->isize <= (32 * 1024 * 1024)) {
                    // If locale is UTF-8 but file doesn't contain valid
                    // UTF-8 and is also fairly small, just assume latin1
                    encoding = "ISO-8859-1";
                } else {
                    // Large files are likely binary; just decode as
                    // UTF-8 to avoid costly charset conversion
                    encoding = "UTF-8";
                }
            } else {
                // Assume encoding is same as locale
                encoding = editor.charset.name;
            }
            if (set_encoding(dec, encoding)) {
                // FIXME: error message?
                set_encoding(dec, "UTF-8");
            }
            return true;
        }
    }

    // ASCII
    return false;
}

static bool detect_and_read_line(FileDecoder *dec, char **linep, size_t *lenp)
{
    char *line = (char *)dec->ibuf + dec->ipos;
    const char *nl = memchr(line, '\n', dec->isize - dec->ipos);
    size_t len;

    if (nl) {
        len = nl - line;
    } else {
        len = dec->isize - dec->ipos;
        if (len == 0) {
            return false;
        }
    }

    if (detect(dec, line, len)) {
        // Encoding detected
        return dec->read_line(dec, linep, lenp);
    }

    // Only ASCII so far
    dec->ipos += len;
    if (nl) {
        dec->ipos++;
    }
    *linep = line;
    *lenp = len;
    return true;
}

FileDecoder *new_file_decoder (
    const char *encoding,
    const unsigned char *buf,
    size_t size
) {
    FileDecoder *dec = xnew0(FileDecoder, 1);
    dec->ibuf = buf;
    dec->isize = size;
    dec->read_line = detect_and_read_line;

    if (encoding) {
        if (set_encoding(dec, encoding)) {
            free_file_decoder(dec);
            return NULL;
        }
    }

    return dec;
}

void free_file_decoder(FileDecoder *dec)
{
    if (dec->cconv != NULL) {
        cconv_free(dec->cconv);
    }
    free(dec);
}

bool file_decoder_read_line(FileDecoder *dec, char **linep, size_t *lenp)
{
    return dec->read_line(dec, linep, lenp);
}
