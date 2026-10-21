/* chd2bin IN.chd OUT.bin : a CD CHD's tracks back to the raw 2352-byte sectors of the original BIN (data first,
   subchannel dropped, each track's CHD padding to 4 frames dropped). Prints a cue line per track. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libchdr/chd.h>
#include <libchdr/cdrom.h>
int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: chd2bin in.chd out.bin\n"); return 2; }
    chd_file *chd; if (chd_open(argv[1], CHD_OPEN_READ, NULL, &chd) != CHDERR_NONE) { fprintf(stderr, "open failed\n"); return 1; }
    const chd_header *h = chd_get_header(chd);
    if (h->unitbytes != CD_FRAME_SIZE) { fprintf(stderr, "not a CD CHD (unit %u)\n", h->unitbytes); return 1; }
    FILE *out = fopen(argv[2], "wb"); if (!out) return 1;
    unsigned char *hunk = malloc(h->hunkbytes); unsigned fph = h->hunkbytes / CD_FRAME_SIZE;
    unsigned cached = (unsigned)-1, chdframe = 0;
    for (int t = 0; ; t++) {
        char meta[256]; unsigned len, tag; unsigned char flags; int trk, frames, pregap = 0, postgap = 0;
        char type[64], sub[64], pgtype[64] = "", pgsub[64] = "";
        if (chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, t, meta, sizeof meta, &len, &tag, &flags) == CHDERR_NONE) {
            meta[len < sizeof meta ? len : sizeof meta - 1] = 0;
            sscanf(meta, CDROM_TRACK_METADATA2_FORMAT, &trk, type, sub, &frames, &pregap, pgtype, pgsub, &postgap);
        } else if (chd_get_metadata(chd, CDROM_TRACK_METADATA_TAG, t, meta, sizeof meta, &len, &tag, &flags) == CHDERR_NONE) {
            meta[len < sizeof meta ? len : sizeof meta - 1] = 0;
            sscanf(meta, CDROM_TRACK_METADATA_FORMAT, &trk, type, sub, &frames);
        } else break;
        if (pregap && pgtype[0] != 'V') { fprintf(stderr, "track %d: a pregap not stored in the CHD is unsupported\n", trk); return 1; }
        printf("TRACK %02d %s FRAMES %d\n", trk, type, frames);
        for (int f = 0; f < frames; f++, chdframe++) {
            unsigned hn = chdframe / fph;
            if (hn != cached) { if (chd_read(chd, hn, hunk) != CHDERR_NONE) { fprintf(stderr, "read failed\n"); return 1; } cached = hn; }
            fwrite(hunk + (chdframe % fph) * CD_FRAME_SIZE, 1, CD_MAX_SECTOR_DATA, out);
        }
        chdframe += (4 - frames % 4) % 4;   /* the CHD pads each track to a multiple of 4 frames */
    }
    fclose(out); chd_close(chd); return 0;
}
