#include "kitty-gfx.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG_MODULE "kitty-gfx"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "grid.h"
#include "base64.h"
#include "render.h"
#include "terminal.h"
#include "util.h"
#include "xmalloc.h"

/* Image data kept per terminal, in bytes; the oldest images go first */
#define KITTY_GFX_MAX_BYTES (320u * 1024 * 1024)
#define KITTY_GFX_MAX_DIM 10000

struct cmd {
    char a, t, d;
    int f, s, v, x, y, w, h, X, Y, z, q;
    uint32_t i, p;
    const char *payload;
    size_t payload_len;
};

static bool
parse(const char *data, size_t len, struct cmd *c)
{
    *c = (struct cmd){.a = 't', .t = 'd', .d = 'a', .f = 32};

    size_t i = 0;
    while (i < len && data[i] != ';') {
        char key = data[i];
        if (i + 1 >= len || data[i + 1] != '=')
            return false;
        i += 2;

        size_t start = i;
        while (i < len && data[i] != ',' && data[i] != ';')
            i++;

        char val[32];
        size_t vlen = min(i - start, sizeof(val) - 1);
        memcpy(val, &data[start], vlen);
        val[vlen] = '\0';
        long n = strtol(val, NULL, 10);

        switch (key) {
        case 'a': c->a = val[0]; break;
        case 't': c->t = val[0]; break;
        case 'd': c->d = val[0]; break;
        case 'f': c->f = n; break;
        case 's': c->s = n; break;
        case 'v': c->v = n; break;
        case 'x': c->x = n; break;
        case 'y': c->y = n; break;
        case 'w': c->w = n; break;
        case 'h': c->h = n; break;
        case 'X': c->X = n; break;
        case 'Y': c->Y = n; break;
        case 'z': c->z = n; break;
        case 'q': c->q = n; break;
        case 'i': c->i = (uint32_t)strtoul(val, NULL, 10); break;
        case 'p': c->p = (uint32_t)strtoul(val, NULL, 10); break;
        default: break; /* C, m, o, S, O, ... : ignored */
        }

        if (i < len && data[i] == ',')
            i++;
    }

    if (i < len && data[i] == ';') {
        c->payload = &data[i + 1];
        c->payload_len = len - i - 1;
    }
    return true;
}

static void
reply(struct terminal *term, const struct cmd *c, const char *err)
{
    if (c->i == 0 || c->q >= 2 || (err == NULL && c->q >= 1))
        return;

    char buf[256];
    const char *msg = err != NULL ? err : "OK";
    int n = c->p != 0
        ? snprintf(buf, sizeof(buf), "\033_Gi=%u,p=%u;%s\033\\", c->i, c->p, msg)
        : snprintf(buf, sizeof(buf), "\033_Gi=%u;%s\033\\", c->i, msg);
    if (n > 0)
        term_to_slave(term, buf, min((size_t)n, sizeof(buf) - 1));
}

static bool
on_alt(const struct terminal *term)
{
    return term->grid == &term->alt;
}

static pixman_region32_t *
dirty(struct terminal *term)
{
    if (!term->kitty_gfx.dirty_init) {
        pixman_region32_init(&term->kitty_gfx.dirty);
        term->kitty_gfx.dirty_init = true;
    }
    return &term->kitty_gfx.dirty;
}

/* A placement's window pixels, clipped to the text area */
static bool
placement_box(const struct terminal *term, const struct kitty_placement *pl,
              pixman_box32_t *b)
{
    const int left = term->margins.left, top = term->margins.top;
    b->x1 = left + pl->col * term->cell_width + pl->xoff;
    b->y1 = top + pl->row * term->cell_height + pl->yoff;
    b->x2 = min(b->x1 + pl->w, left + term->cols * term->cell_width);
    b->y2 = min(b->y1 + pl->h, top + term->rows * term->cell_height);
    return b->x1 < b->x2 && b->y1 < b->y2;
}

/* Marks a placement's pixels (its old or new position) for redrawing */
static void
damage_placement(struct terminal *term, const struct kitty_placement *pl)
{
    pixman_box32_t b;
    if (pl->alt != on_alt(term) || !placement_box(term, pl, &b))
        return;
    pixman_region32_union_rect(dirty(term), dirty(term),
                               b.x1, b.y1, b.x2 - b.x1, b.y2 - b.y1);
}

static struct kitty_image *
find_image(struct terminal *term, uint32_t id)
{
    tll_foreach(term->kitty_gfx.images, it) {
        if (it->item.id == id)
            return &it->item;
    }
    return NULL;
}

static void
delete_placements(struct terminal *term, uint32_t image_id, uint32_t id,
                  bool all)
{
    tll_foreach(term->kitty_gfx.placements, it) {
        struct kitty_placement *pl = &it->item;
        if (pl->alt != on_alt(term))
            continue;
        if (!all && (pl->image_id != image_id || (id != 0 && pl->id != id)))
            continue;
        damage_placement(term, pl);
        tll_remove(term->kitty_gfx.placements, it);
    }
}

static void
image_free(struct terminal *term, struct kitty_image *img)
{
    tll_foreach(term->kitty_gfx.placements, it) {
        if (it->item.image_id == img->id && it->item.alt == img->alt) {
            damage_placement(term, &it->item);
            tll_remove(term->kitty_gfx.placements, it);
        }
    }
    term->kitty_gfx.bytes -= (size_t)img->width * img->height * 4;
    pixman_image_unref(img->pix);
    free(img->data);
}

static void
delete_images(struct terminal *term, uint32_t id, bool all)
{
    tll_foreach(term->kitty_gfx.images, it) {
        if (it->item.alt != on_alt(term) || (!all && it->item.id != id))
            continue;
        image_free(term, &it->item);
        tll_remove(term->kitty_gfx.images, it);
    }
}

/* Frees images without placements, used by d=A and d=I */
static void
delete_unplaced_images(struct terminal *term)
{
    tll_foreach(term->kitty_gfx.images, it) {
        bool placed = false;
        tll_foreach(term->kitty_gfx.placements, p) {
            if (p->item.image_id == it->item.id && p->item.alt == it->item.alt) {
                placed = true;
                break;
            }
        }
        if (!placed && it->item.alt == on_alt(term)) {
            image_free(term, &it->item);
            tll_remove(term->kitty_gfx.images, it);
        }
    }
}

/* Loads the pixels of a transmit command into a new ARGB image */
static const char *
load(struct terminal *term, const struct cmd *c, uint32_t **out)
{
    if (c->f != 24 && c->f != 32)
        return "EINVAL:unsupported format";
    if (c->s <= 0 || c->v <= 0 || c->s > KITTY_GFX_MAX_DIM || c->v > KITTY_GFX_MAX_DIM)
        return "EINVAL:bad size";

    const size_t bpp = c->f / 8;
    const size_t need = (size_t)c->s * c->v * bpp;
    const uint8_t *src = NULL;
    void *map = MAP_FAILED;
    size_t map_len = 0;
    char *decoded = NULL;

    if (c->t == 's') {
        char name[256];
        size_t nlen = 0;
        char *n = base64_decode(c->payload != NULL ? c->payload : "", &nlen);
        if (n == NULL || nlen == 0 || nlen >= sizeof(name)) {
            free(n);
            return "EINVAL:bad shared memory name";
        }
        memcpy(name, n, nlen);
        name[nlen] = '\0';
        free(n);

        int fd = shm_open(name, O_RDONLY, 0);
        if (fd < 0)
            return "ENOENT:no such shared memory object";
        struct stat st;
        if (fstat(fd, &st) < 0 || (size_t)st.st_size < need) {
            close(fd);
            shm_unlink(name);
            return "EINVAL:shared memory too small";
        }
        map_len = need;
        map = mmap(NULL, map_len, PROT_READ, MAP_SHARED, fd, 0);
        close(fd);
        shm_unlink(name);
        if (map == MAP_FAILED)
            return "EIO:mmap failed";
        src = map;
    } else if (c->t == 'd') {
        size_t dlen = 0;
        decoded = base64_decode(c->payload != NULL ? c->payload : "", &dlen);
        if (decoded == NULL || dlen < need) {
            free(decoded);
            return "EINVAL:not enough data (chunked and compressed data are not supported)";
        }
        src = (const uint8_t *)decoded;
    } else
        return "EINVAL:unsupported transmission medium";

    uint32_t *data = xmalloc((size_t)c->s * c->v * 4);
    const size_t n = (size_t)c->s * c->v;
    if (bpp == 3) {
        for (size_t k = 0; k < n; k++, src += 3)
            data[k] = 0xffu << 24 | (uint32_t)src[0] << 16 | (uint32_t)src[1] << 8 | src[2];
    } else {
        for (size_t k = 0; k < n; k++, src += 4) {
            uint32_t a = src[3];
            data[k] = a << 24 |
                      (src[0] * a / 255) << 16 |
                      (src[1] * a / 255) << 8 |
                      (src[2] * a / 255);
        }
    }

    if (map != MAP_FAILED)
        munmap(map, map_len);
    free(decoded);
    *out = data;
    return NULL;
}

static const char *
transmit(struct terminal *term, const struct cmd *c)
{
    uint32_t *data;
    const char *err = load(term, c, &data);
    if (err != NULL)
        return err;

    delete_images(term, c->i, false);  /* a new image with the same id replaces the old one */

    struct kitty_image img = {
        .id = c->i,
        .alt = on_alt(term),
        .width = c->s,
        .height = c->v,
        .opaque = c->f == 24,
        .data = data,
        .pix = pixman_image_create_bits_no_clear(
            c->f == 24 ? PIXMAN_x8r8g8b8 : PIXMAN_a8r8g8b8,
            c->s, c->v, data, c->s * 4),
    };
    term->kitty_gfx.bytes += (size_t)c->s * c->v * 4;
    tll_push_back(term->kitty_gfx.images, img);

    /* Over the limit: the oldest images go (never the new one) */
    while (term->kitty_gfx.bytes > KITTY_GFX_MAX_BYTES &&
           tll_length(term->kitty_gfx.images) > 1)
    {
        struct kitty_image *old = &tll_front(term->kitty_gfx.images);
        image_free(term, old);
        tll_pop_front(term->kitty_gfx.images);
    }
    return NULL;
}

static const char *
place(struct terminal *term, const struct cmd *c)
{
    const struct kitty_image *img = find_image(term, c->i);
    if (img == NULL || img->alt != on_alt(term))
        return "ENOENT:no such image";

    int x = max(0, c->x), y = max(0, c->y);
    if (x >= img->width || y >= img->height)
        return "EINVAL:source rectangle outside the image";
    int w = c->w > 0 ? min(c->w, img->width - x) : img->width - x;
    int h = c->h > 0 ? min(c->h, img->height - y) : img->height - y;

    if (c->X < 0 || c->Y < 0 ||
        c->X >= term->cell_width || c->Y >= term->cell_height)
        return "EINVAL:cell offset larger than a cell";

    struct kitty_placement pl = {
        .image_id = c->i,
        .id = c->p,
        .alt = on_alt(term),
        .row = term->grid->cursor.point.row,
        .col = term->grid->cursor.point.col,
        .x = x, .y = y, .w = w, .h = h,
        .xoff = c->X, .yoff = c->Y,
        .z = c->z,
    };

    /* Re-placing (same image and placement id) moves the placement. Unlike
     * kitty, a placement without an id replaces all of the image's ones. */
    delete_placements(term, c->i, c->p, false);
    tll_push_back(term->kitty_gfx.placements, pl);
    damage_placement(term, &tll_back(term->kitty_gfx.placements));
    return NULL;
}

void
kitty_gfx_apc(struct terminal *term, const char *data, size_t len)
{
    if (len == 0 || data[0] != 'G')
        return;

    struct cmd c;
    if (!parse(data + 1, len - 1, &c))
        return;

    const char *err = NULL;
    switch (c.a) {
    case 't':
        err = transmit(term, &c);
        break;

    case 'T':
        err = transmit(term, &c);
        if (err == NULL)
            err = place(term, &c);
        break;

    case 'p':
        err = place(term, &c);
        break;

    case 'q': {
        uint32_t *px;
        err = load(term, &c, &px);
        if (err == NULL)
            free(px);
        break;
    }

    case 'd':
        switch (c.d) {
        case 'a': delete_placements(term, 0, 0, true); break;
        case 'A': delete_placements(term, 0, 0, true); delete_unplaced_images(term); break;
        case 'i': delete_placements(term, c.i, c.p, false); break;
        case 'I':
            delete_placements(term, c.i, c.p, false);
            if (c.p == 0)
                delete_images(term, c.i, false);
            else
                delete_unplaced_images(term);
            break;
        default: break;
        }
        return;  /* deletes never reply */

    default:
        err = "EINVAL:unsupported action";
        break;
    }

    reply(term, &c, err);
    render_refresh(term);
}

static bool
visible(const struct terminal *term, const struct kitty_placement *pl)
{
    return pl->alt == on_alt(term) && pl->z >= 0;
}

void
kitty_gfx_prepare(struct terminal *term, bool scrolled)
{
    if (tll_length(term->kitty_gfx.placements) == 0 &&
        !pixman_region32_not_empty(dirty(term)))
        return;

    const int left = term->margins.left, top = term->margins.top;
    const int cw = term->cell_width, ch = term->cell_height;

    /* A scroll moved the images' pixels along with the text: redraw all */
    if (scrolled && tll_length(term->kitty_gfx.placements) > 0) {
        pixman_region32_union_rect(dirty(term), dirty(term), left, top,
                                   term->cols * cw, term->rows * ch);
    }

    /* Pixels an opaque image will cover completely */
    pixman_region32_t cover;
    pixman_region32_init(&cover);
    tll_foreach(term->kitty_gfx.placements, it) {
        const struct kitty_placement *pl = &it->item;
        const struct kitty_image *img = find_image(term, pl->image_id);
        pixman_box32_t b;
        if (visible(term, pl) && img != NULL && img->opaque &&
            placement_box(term, pl, &b))
        {
            pixman_region32_union_rect(&cover, &cover, b.x1, b.y1,
                                       b.x2 - b.x1, b.y2 - b.y1);
        }
    }

    /* What an image has left, or what lies under a translucent one, is text */
    pixman_region32_t exposed;
    pixman_region32_init(&exposed);
    pixman_region32_subtract(&exposed, dirty(term), &cover);

    const pixman_box32_t *ce = pixman_region32_extents(&cover);
    const pixman_box32_t *ee = pixman_region32_extents(&exposed);
    const bool any_cover = pixman_region32_not_empty(&cover);
    const bool any_exposed = pixman_region32_not_empty(&exposed);
    bool hid = false;

    for (int r = 0; r < term->rows; r++) {
        const int y = top + r * ch;
        const bool in_exposed = any_exposed && y < ee->y2 && y + ch > ee->y1;
        const bool in_cover = any_cover && y < ce->y2 && y + ch > ce->y1;
        if (!in_exposed && !in_cover)
            continue;

        struct row *row = grid_row_in_view(term->grid, r);
        if (!in_exposed && !row->dirty)
            continue;

        for (int c = 0; c < term->cols; c++) {
            struct cell *cell = &row->cells[c];
            pixman_box32_t b = {left + c * cw, y, left + (c + 1) * cw, y + ch};

            if (in_exposed &&
                pixman_region32_contains_rectangle(&exposed, &b) != PIXMAN_REGION_OUT)
            {
                cell->attrs.clean = 0;
                row->dirty = true;
            } else if (in_cover && !cell->attrs.clean &&
                       pixman_region32_contains_rectangle(&cover, &b) == PIXMAN_REGION_IN)
            {
                /* Hidden: not rendered, but drawn over by its image */
                cell->attrs.clean = 1;
                hid = true;
            }
        }
    }

    /* One region, not one box per cell: it goes on to the compositor */
    if (hid)
        pixman_region32_union(dirty(term), dirty(term), &cover);

    pixman_region32_fini(&exposed);
    pixman_region32_fini(&cover);
}

void
kitty_gfx_render(struct terminal *term, pixman_image_t *pix,
                 pixman_region32_t *damage)
{
    if (!term->kitty_gfx.dirty_init)
        return;

    /* Placements are drawn wherever this frame changed: over re-rendered
     * text, and where they moved to or from. The rest of the buffer was
     * copied from the previous frame, placements included. */
    pixman_region32_union(damage, damage, dirty(term));
    pixman_region32_clear(dirty(term));

    if (tll_length(term->kitty_gfx.placements) == 0)
        return;

    pixman_image_set_clip_region32(pix, damage);

    tll_foreach(term->kitty_gfx.placements, it) {
        const struct kitty_placement *pl = &it->item;
        const struct kitty_image *img;
        pixman_box32_t b;
        if (!visible(term, pl) || !placement_box(term, pl, &b) ||
            (img = find_image(term, pl->image_id)) == NULL)
            continue;

        pixman_image_composite32(
            img->opaque ? PIXMAN_OP_SRC : PIXMAN_OP_OVER,
            img->pix, NULL, pix,
            pl->x, pl->y, 0, 0, b.x1, b.y1, b.x2 - b.x1, b.y2 - b.y1);
    }

    pixman_image_set_clip_region32(pix, NULL);
}

static void
free_all(struct terminal *term, bool alt_only)
{
    tll_foreach(term->kitty_gfx.placements, it) {
        if (!alt_only || it->item.alt)
            tll_remove(term->kitty_gfx.placements, it);
    }
    tll_foreach(term->kitty_gfx.images, it) {
        if (alt_only && !it->item.alt)
            continue;
        term->kitty_gfx.bytes -= (size_t)it->item.width * it->item.height * 4;
        pixman_image_unref(it->item.pix);
        free(it->item.data);
        tll_remove(term->kitty_gfx.images, it);
    }
}

void
kitty_gfx_reset(struct terminal *term)
{
    free_all(term, false);
    if (term->kitty_gfx.dirty_init)
        pixman_region32_fini(&term->kitty_gfx.dirty);
    term->kitty_gfx.dirty_init = false;
}

void
kitty_gfx_leave_alt(struct terminal *term)
{
    free_all(term, true);
}
