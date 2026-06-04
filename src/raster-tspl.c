/*
 * raster-tspl.c - CUPS raster filter for TSPL thermal label printers
 *
 * Converts CUPS raster input to TSPL (Thermal/Shipping/Printer Language)
 * commands for Blueprint BP-TD110 and compatible printers.
 *
 * Build: gcc -o raster-tspl raster-tspl.c -lcups -lcupsimage
 * Or on newer CUPS (no separate cupsimage): gcc -o raster-tspl raster-tspl.c -lcups
 *
 * CUPS filter arguments:
 *   argv[1]  job-id
 *   argv[2]  user
 *   argv[3]  title
 *   argv[4]  copies
 *   argv[5]  options (space-separated key=value pairs)
 *   argv[6]  filename (optional; reads stdin if absent)
 */

#include <cups/cups.h>
#include <cups/raster.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

/* ---- helpers ---- */

static int opt_int(cups_option_t *opts, int n, const char *name, int def) {
    const char *v = cupsGetOption(name, n, opts);
    return (v && strcmp(v, "None") != 0) ? atoi(v) : def;
}

static const char *opt_str(cups_option_t *opts, int n, const char *name, const char *def) {
    const char *v = cupsGetOption(name, n, opts);
    return (v && strcmp(v, "None") != 0) ? v : def;
}

/* Reverse all bits in a byte (for mirror mode) */
static unsigned char reverse_byte(unsigned char b) {
    b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
    b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
    b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
    return b;
}

/* Mirror a packed 1-bit row in-place */
static void mirror_row(unsigned char *row, int width_bytes, int width_pixels) {
    /* Reverse byte order and reverse bits within each byte */
    int lo = 0, hi = width_bytes - 1;
    while (lo <= hi) {
        unsigned char tmp = reverse_byte(row[lo]);
        row[lo] = reverse_byte(row[hi]);
        row[hi] = tmp;
        lo++; hi--;
    }
    /* If pixel width is not a multiple of 8, we need to shift right by the slack */
    int slack = width_bytes * 8 - width_pixels;
    if (slack > 0) {
        for (int i = width_bytes - 1; i >= 0; i--) {
            unsigned char carry = (i > 0) ? (row[i - 1] << (8 - slack)) : 0;
            row[i] = (row[i] >> slack) | carry;
        }
    }
}

/* ---- main filter ---- */

int main(int argc, char *argv[]) {
    if (argc < 6 || argc > 7) {
        fputs("Usage: raster-tspl job user title copies options [filename]\n", stderr);
        return 1;
    }

    /* Ignore SIGPIPE so we can detect write errors ourselves */
    signal(SIGPIPE, SIG_IGN);

    /* Parse job options from argv[5] */
    cups_option_t *jobopts = NULL;
    int njobopts = cupsParseOptions(argv[5], 0, &jobopts);

    /* Open input */
    int fd = STDIN_FILENO;
    if (argc == 7) {
        fd = open(argv[6], O_RDONLY);
        if (fd < 0) {
            perror("raster-tspl: open");
            return 1;
        }
    }

    cups_raster_t *ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    if (!ras) {
        fprintf(stderr, "raster-tspl: cupsRasterOpen failed\n");
        if (fd != STDIN_FILENO) close(fd);
        return 1;
    }

    /* Make stdout unbuffered for raw binary output */
    setbuf(stdout, NULL);

    int page = 0;
    cups_page_header2_t hdr;

    while (cupsRasterReadHeader2(ras, &hdr)) {
        page++;

        unsigned int W        = hdr.cupsWidth;        /* dots */
        unsigned int H        = hdr.cupsHeight;       /* dots */
        unsigned int bpp      = hdr.cupsBitsPerPixel; /* 1 or 8 */
        unsigned int bpl      = hdr.cupsBytesPerLine; /* CUPS bytes per row (may be padded) */
        unsigned int res_x    = hdr.HWResolution[0];  /* DPI */
        unsigned int res_y    = hdr.HWResolution[1];

        /* Label physical size in mm */
        float width_mm  = (float)W * 25.4f / (float)res_x;
        float height_mm = (float)H * 25.4f / (float)res_y;

        /* TSPL bitmap row width (bytes) */
        int tspl_bpr = (int)(W + 7) / 8;

        /* Job options (with sensible defaults) */
        int   speed    = opt_int(jobopts, njobopts, "PrintSpeed",    4);
        int   density  = opt_int(jobopts, njobopts, "PrintDarkness", 8);
        int   shift    = opt_int(jobopts, njobopts, "ShiftMove",     0);
        int   offset   = opt_int(jobopts, njobopts, "FowardOffset",  0); /* PPD typo preserved */
        const char *ptype  = opt_str(jobopts, njobopts, "PaperType",    "1");
        int   do_mirror    = strcmp(opt_str(jobopts, njobopts, "MirrorImage",   "False"), "True") == 0;
        int   do_negative  = strcmp(opt_str(jobopts, njobopts, "NegativeImage", "False"), "True") == 0;

        /* Clamp speed and density to sane ranges */
        if (speed < 2)   speed = 2;
        if (speed > 6)   speed = 6;
        if (density < 0) density = 0;
        if (density > 15) density = 15;

        /* ---- TSPL page-setup commands ---- */
        fprintf(stdout, "SIZE %.1f mm, %.1f mm\r\n", width_mm, height_mm);

        if (strcmp(ptype, "2") == 0)
            fputs("BLINE 2 mm, 0 mm\r\n", stdout);   /* black-mark sensing */
        else if (strcmp(ptype, "3") == 0)
            fputs("GAP 0 mm, 0 mm\r\n", stdout);     /* continuous paper */
        else
            fputs("GAP 2 mm, 0 mm\r\n", stdout);     /* gap sensing (default) */

        fputs("SET RIBBON OFF\r\n", stdout);          /* direct thermal, no ribbon */
        fprintf(stdout, "DENSITY %d\r\n", density);
        fprintf(stdout, "SPEED %d\r\n", speed);

        if (shift != 0)  fprintf(stdout, "SHIFT %d\r\n",     shift);
        if (offset != 0) fprintf(stdout, "OFFSET %d mm\r\n", offset);

        fputs("SET TEAR ON\r\n",   stdout);
        fputs("SET PEEL OFF\r\n",  stdout);
        fputs("SET CUTTER OFF\r\n", stdout);
        fputs("CLS\r\n", stdout);   /* clear image buffer */
        fflush(stdout);

        /* ---- Allocate buffers ---- */
        unsigned char *cup_row  = malloc(bpl);           /* one CUPS row */
        unsigned char *tspl_buf = calloc(tspl_bpr * (int)H, 1); /* full page, 1-bit */

        if (!cup_row || !tspl_buf) {
            fputs("raster-tspl: out of memory\n", stderr);
            free(tspl_buf);
            /* Drain remaining pixel data to keep stream in sync, reusing H as a 4-byte sink */
            unsigned char *drain = cup_row ? cup_row : (unsigned char *)&H;
            for (unsigned int y = 0; y < H; y++)
                cupsRasterReadPixels(ras, drain, cup_row ? bpl : sizeof(H));
            free(cup_row);
            continue;
        }

        /* ---- Read and convert pixel data ---- */
        for (unsigned int y = 0; y < H; y++) {
            if (cupsRasterReadPixels(ras, cup_row, bpl) == 0) {
                fprintf(stderr, "raster-tspl: short read at row %u (page %d)\n", y, page);
                break;
            }

            unsigned char *dst = tspl_buf + (size_t)y * tspl_bpr;

            if (bpp == 1) {
                /*
                 * 1-bit packed. CUPS raster for thermal label printers is
                 * delivered in luminance convention (CS_W semantics) even when
                 * cupsColorSpace=3 (CS_K) is set: bit 1 = white, bit 0 = black.
                 * TSPL BITMAP wants the opposite: bit 1 = print (black).
                 * Invert every byte before sending.
                 */
                int copy = tspl_bpr < (int)bpl ? tspl_bpr : (int)bpl;
                for (int b = 0; b < copy; b++)
                    dst[b] = ~cup_row[b];

            } else {
                /*
                 * 8-bit grayscale, CS_K: byte 255 = full ink (black).
                 * Threshold at mid-point and pack to 1-bit.
                 */
                memset(dst, 0, tspl_bpr);
                for (unsigned int x = 0; x < W; x++) {
                    if (cup_row[x] < 128) {       /* CS_W: 0=black → print */
                        dst[x >> 3] |= (unsigned char)(0x80u >> (x & 7));
                    }
                }
            }

            /* Apply effects per-row */
            if (do_negative) {
                for (int b = 0; b < tspl_bpr; b++)
                    dst[b] ^= 0xFFu;
            }
            if (do_mirror) {
                mirror_row(dst, tspl_bpr, (int)W);
            }
        }

        /* ---- TSPL BITMAP command ---- */
        /*
         * Format: BITMAP x,y,width_bytes,height,mode,<binary_data>\r\n
         *   mode 1 = OR (standard for full-page write)
         * The printer reads exactly width_bytes*height bytes of binary data
         * after the trailing comma; the \r\n terminates the command.
         *
         * x_offset: horizontal start in dots. The BP-TD110 printhead has a
         * small dead zone at x=0; shifting right by ~16 dots (2 mm at 203 DPI)
         * centres the image on the label. Adjust via PrintXOffset CUPS option.
         */
        int x_offset = opt_int(jobopts, njobopts, "PrintXOffset", 16);
        fprintf(stdout, "BITMAP %d,0,%d,%d,1,", x_offset, tspl_bpr, (int)H);
        fwrite(tspl_buf, (size_t)tspl_bpr * H, 1, stdout);
        fputs("\r\n", stdout);

        /* Print: 1 label, 1 copy */
        fputs("PRINT 1,1\r\n", stdout);
        fflush(stdout);

        free(cup_row);
        free(tspl_buf);
    }

    cupsRasterClose(ras);
    if (fd != STDIN_FILENO) close(fd);
    cupsFreeOptions(njobopts, jobopts);

    if (page == 0) {
        fputs("raster-tspl: no pages found in input\n", stderr);
        return 1;
    }
    return 0;
}
