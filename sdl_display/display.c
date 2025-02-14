/*
 * This file is part of AtomGL.
 *
 * Copyright 2021-2023 Davide Bettio <davide@uninstall.it>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#include <context.h>
#include <defaultatoms.h>
#include <interop.h>
#include <mailbox.h>
#include <port.h>
#include <term.h>
#include <utils.h>

#include "ufontlib.h"

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define BPP 4
#define DEPTH 32

#define CHAR_WIDTH 8
#include "../display_items.h"
#include "../font.c"
#include "../image_helpers.h"

struct DisplayOpts
{
    avm_int_t width;
    avm_int_t height;
};

struct KeyboardEvent
{
    uint16_t key;
    uint16_t unicode;
    bool key_down;
};

struct MouseEvent
{
    int type;
    int button;
    int x;
    int y;
};

struct Rectangle
{
    int x;
    int y;
    int width;
    int height;
    bool valid;
};

static term keyboard_pid;
static struct timespec ts0;
Context *the_ctx;

struct Screen
{
    int w;
    int h;
    int scale;
    void *pixels;
    SDL_PixelFormat *format;
};

static struct Screen *screen;
static SDL_Surface *surface;
static pthread_mutex_t ready_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;

UFontManager *ufont_manager;

static NativeHandlerResult consume_display_mailbox(Context *ctx);
static void *display_loop();

Message *prev_message = NULL;
BaseDisplayItem *prev_items = NULL;
int prev_items_len = 0;

static void destroy_message(Message *m, GlobalContext *global)
{
    BEGIN_WITH_STACK_HEAP(1, temp_heap);
    mailbox_message_dispose(&m->base, &temp_heap);
    END_WITH_STACK_HEAP(temp_heap, global);
}

static inline int int_min(int a, int b)
{
    return (a > b) ? b : a;
}

static inline int int_max(int a, int b)
{
    return (a > b) ? a : b;
}

static bool cmp_display_item(BaseDisplayItem *a, BaseDisplayItem *b)
{
    if (a->primitive != b->primitive || a->x != b->x || a->y != b->y ||
            a->width != b->width || a->height != b->height || a->brcolor != b->brcolor) {
        return false;
    }

    switch (a->primitive) {
        case Image:
            return a->data.image_data.pix == b->data.image_data.pix;

        case Rect:
            return true;

        case Text:
            return (a->data.text_data.fgcolor == b->data.text_data.fgcolor) &&
                !strcmp(a->data.text_data.text, b->data.text_data.text);

        case ScaledCroppedImage:
            return (a->data.image_data.pix == b->data.image_data.pix) &&
                (a->x_scale == b->x_scale) && (a->y_scale == b->y_scale) &&
                (a->source_x == b->source_x) && (a->source_y == b->source_y);

        default: {
            return true;
        }
    }
}

static void update_damaged_area(struct Rectangle *area, const struct Rectangle *damage)
{
    if (area->valid) {
        area->x = int_min(area->x, damage->x);
        area->y = int_min(area->y, damage->y);
        area->width = int_max(area->x + area->width, damage->x + damage->width) - area->x;
        area->height = int_max(area->y + area->height, damage->y + damage->height) - area->y;
    } else {
        area->x = damage->x;
        area->y = damage->y;
        area->width = damage->width;
        area->height = damage->height;
        area->valid = true;
    }
}

static void clip_rectangle(struct Rectangle *rectangle, const struct Rectangle *clip_region)
{
    rectangle->x = int_max(rectangle->x, clip_region->x);
    rectangle->y = int_max(rectangle->y, clip_region->y);
    rectangle->width = int_min(rectangle->x + rectangle->width, clip_region->x + clip_region->width) - rectangle->x;
    rectangle->height = int_min(rectangle->y + rectangle->height, clip_region->y + clip_region->height) - rectangle->y;
}

static void dumb_diff(BaseDisplayItem *orig, int orig_len, BaseDisplayItem *new, int new_len, struct Rectangle *damaged)
{
    if (orig_len == 0) {
        for (int i = 0; i < new_len; i++) {
            struct Rectangle irect = {
                .x = new[i].x,
                .y = new[i].y,
                .width = new[i].width,
                .height = new[i].height,
                .valid = true
            };
            update_damaged_area(damaged, &irect);
        }
        return;
    }

    int j = 0;

    for (int i = 0; i < new_len; i++) {
        if (cmp_display_item(&new[i], &orig[j])) {
            j++;
        } else {
            bool found = false;
            for (int k = j + 1; k < orig_len; k++) {
                if (cmp_display_item(&new[i], &orig[k])) {
                    for (int l = k - j; l < k; l++) {
                        struct Rectangle irect = {
                            .x = orig[l].x,
                            .y = orig[l].y,
                            .width = orig[l].width,
                            .height = orig[l].height,
                            .valid = true
                        };
                        update_damaged_area(damaged, &irect);
                    }

                    j = k + 1;
                    found = true;
                    break;
                }
            }
            if (!found) {
                struct Rectangle irect = {
                    .x = new[i].x,
                    .y = new[i].y,
                    .width = new[i].width,
                    .height = new[i].height,
                    .valid = true
                };
                update_damaged_area(damaged, &irect);
            }
        }
    }
}

static inline Uint32 uint32_color_to_surface(struct Screen *screen, uint32_t color)
{
    return SDL_MapRGB(screen->format, (color >> 24) & 0xFF, (color >> 16) & 0xFF, (color >> 8) & 0xFF);
}

struct Surface
{
    int width;
    int height;
    void *buffer;
};

void epd_draw_pixel(int xpos, int ypos, uint8_t color, void *buffer)
{
    struct Surface *surface = buffer;

    if (xpos < 0 || ypos < 0 || xpos >= surface->width || ypos >= surface->height) {
        return;
    }

    Uint32 *pixmem32b = (Uint32 *) (((uint8_t *) surface->buffer) + surface->width * ypos * BPP + xpos * BPP);

    //TODO: handle other colors than black
    UNUSED(color);
    *pixmem32b = 0xFF000000;
}

static int draw_image_x(int xpos, int ypos, int max_line_len, BaseDisplayItem *item)
{
    int x = item->x;
    int y = item->y;

    Uint32 bgcolor;
    bool visible_bg;
    if (item->brcolor != 0) {
        bgcolor = uint32_color_to_surface(screen, item->brcolor);
        visible_bg = true;
    } else {
        visible_bg = false;
    }

    int width = item->width;
    const char *data = item->data.image_data.pix;

    int drawn_pixels = 0;

    uint32_t *pixels = ((uint32_t *) data) + (ypos - y) * width + (xpos - x);
    Uint32 *pixmem32 = (Uint32 *) (((uint8_t *) screen->pixels) + screen->w * ypos * BPP + xpos * BPP);

    if (width > xpos - x + max_line_len) {
        width = xpos - x + max_line_len;
    }

    for (int j = xpos - x; j < width; j++) {
        uint32_t img_pixel = READ_32_UNALIGNED(pixels);
        if ((*pixels >> 24) & 0xFF) {
            Uint32 color = uint32_color_to_surface(screen, img_pixel);
            pixmem32[drawn_pixels] = color;
        } else if (visible_bg) {
            pixmem32[drawn_pixels] = bgcolor;
        } else {
            return drawn_pixels;
        }
        drawn_pixels++;
        pixels++;
    }

    return drawn_pixels;
}

static int draw_scaled_cropped_img_x(int xpos, int ypos, int max_line_len, BaseDisplayItem *item)
{
    int x = item->x;
    int y = item->y;

    Uint32 bgcolor;
    bool visible_bg;
    if (item->brcolor != 0) {
        bgcolor = uint32_color_to_surface(screen, item->brcolor);
        visible_bg = true;
    } else {
        visible_bg = false;
    }

    int width = item->width;
    const char *data = item->data.image_data_with_size.pix;

    int drawn_pixels = 0;

    int y_scale = item->y_scale;
    int x_scale = item->x_scale;
    int img_width = item->data.image_data_with_size.width;

    int source_x = item->source_x;
    int source_y = item->source_y;

    uint32_t *pixels = ((uint32_t *) data) + (source_y + ((ypos - y) / y_scale)) * img_width + source_x + ((xpos - x) / x_scale);
    Uint32 *pixmem32 = (Uint32 *) (((uint8_t *) screen->pixels) + screen->w * ypos * BPP + xpos * BPP);

    if (source_x + (width / x_scale) > img_width) {
        width = (img_width - source_x) * x_scale;
    }

    if (width > xpos - x + max_line_len) {
        width = xpos - x + max_line_len;
    }

    for (int j = xpos - x; j < width; j++) {
        uint32_t img_pixel = READ_32_UNALIGNED(pixels);
        if ((*pixels >> 24) & 0xFF) {
            Uint32 color = uint32_color_to_surface(screen, img_pixel);
            pixmem32[drawn_pixels] = color;
        } else if (visible_bg) {
            pixmem32[drawn_pixels] = bgcolor;
        } else {
            return drawn_pixels;
        }
        drawn_pixels++;
        pixels = ((uint32_t *) data) + (source_y + ((ypos - y) / y_scale)) * img_width + source_x + (j / x_scale);
    }

    return drawn_pixels;
}

static int draw_rect_x(int xpos, int ypos, int max_line_len, BaseDisplayItem *item)
{
    int x = item->x;
    int width = item->width;
    uint32_t color = uint32_color_to_surface(screen, item->brcolor);

    int drawn_pixels = 0;

    Uint32 *pixmem32 = (Uint32 *) (((uint8_t *) screen->pixels) + screen->w * ypos * BPP + xpos * BPP);

    if (width > xpos - x + max_line_len) {
        width = xpos - x + max_line_len;
    }

    for (int j = xpos - x; j < width; j++) {
        pixmem32[drawn_pixels] = color;
        drawn_pixels++;
    }

    return drawn_pixels;
}

static int draw_text_x(int xpos, int ypos, int max_line_len, BaseDisplayItem *item)
{
    int x = item->x;
    int y = item->y;
    Uint32 fgcolor = uint32_color_to_surface(screen, item->data.text_data.fgcolor);
    Uint32 bgcolor;
    bool visible_bg;
    if (item->brcolor != 0) {
        bgcolor = uint32_color_to_surface(screen, item->brcolor);
        visible_bg = true;
    } else {
        visible_bg = false;
    }

    char *text = (char *) item->data.text_data.text;

    int width = item->width;

    int drawn_pixels = 0;

    Uint32 *pixmem32 = (Uint32 *) (((uint8_t *) screen->pixels) + screen->w * ypos * BPP + xpos * BPP);

    if (width > xpos - x + max_line_len) {
        width = xpos - x + max_line_len;
    }

    for (int j = xpos - x; j < width; j++) {
        int char_index = j / CHAR_WIDTH;
        char c = text[char_index];
        unsigned const char *glyph = fontdata + ((unsigned char) c) * 16;

        unsigned char row = glyph[ypos - y];

        bool opaque;
        int k = j % CHAR_WIDTH;
        if (row & (1 << (7 - k))) {
            opaque = true;
        } else {
            opaque = false;
        }

        if (opaque) {
            pixmem32[drawn_pixels] = fgcolor;
        } else if (visible_bg) {
            pixmem32[drawn_pixels] = bgcolor;
        } else {
            return drawn_pixels;
        }
        drawn_pixels++;
    }

    return drawn_pixels;
}

static int find_max_line_len(BaseDisplayItem *items, int count, int xpos, int ypos)
{
    int line_len = screen->w;

    for (int i = 0; i < count; i++) {
        BaseDisplayItem *item = &items[i];

        if ((xpos < item->x) && (ypos >= item->y) && (ypos < item->y + item->height)) {
            int len_to_item = item->x - xpos;
            line_len = (line_len > len_to_item) ? len_to_item : line_len;
        }
    }

    return line_len;
}

static int draw_x(int xpos, int ypos, BaseDisplayItem *items, int items_count)
{
    bool below = false;

    for (int i = 0; i < items_count; i++) {
        BaseDisplayItem *item = &items[i];
        if ((xpos < item->x) || (xpos >= item->x + item->width) || (ypos < item->y) || (ypos >= item->y + item->height)) {
            continue;
        }

        int max_line_len = below ? 1 : find_max_line_len(items, i, xpos, ypos);

        int drawn_pixels = 0;
        switch (items[i].primitive) {
            case Image:
                drawn_pixels = draw_image_x(xpos, ypos, max_line_len, item);
                break;

            case ScaledCroppedImage:
                drawn_pixels = draw_scaled_cropped_img_x(xpos, ypos, max_line_len, item);
                break;

            case Rect:
                drawn_pixels = draw_rect_x(xpos, ypos, max_line_len, item);
                break;

            case Text:
                drawn_pixels = draw_text_x(xpos, ypos, max_line_len, item);
                break;

            default: {
                fprintf(stderr, "unexpected display list command.\n");
            }
        }

        if (drawn_pixels != 0) {
            return drawn_pixels;
        }

        below = true;
    }

    return 1;
}

static void do_update(Context *ctx, term display_list)
{
    int proper;
    int len = term_list_length(display_list, &proper);

    BaseDisplayItem *items = malloc(sizeof(BaseDisplayItem) * len);

    term t = display_list;
    for (int i = 0; i < len; i++) {
        init_item(&items[i], term_get_list_head(t), ctx);
        t = term_get_list_tail(t);
    }

    struct Rectangle damaged;
    damaged.valid = false;
    dumb_diff(prev_items, prev_items_len, items, len, &damaged);
    if (prev_items) {
        destroy_items(prev_items, prev_items_len);
        destroy_message(prev_message, ctx->global);
    }
    prev_items = items;
    prev_items_len = len;

    if (!damaged.valid) {
        // skip update
        return;
    }

    struct Rectangle screen_rect = {
        .x = 0,
        .y = 0,
        .width = screen->w,
        .height = screen->h,
        .valid = true
    };
    clip_rectangle(&damaged, &screen_rect);

    // BUG: damage area is not correct
    // WORKAROUND: always damage the whole screen to force full redraw
    damaged.x = 0;
    damaged.y = 0;
    damaged.height = screen->h;
    damaged.width = screen->w;
    // END OF WORKAROUND

    for (int ypos = damaged.y; ypos < damaged.y + damaged.height; ypos++) {
        int xpos = damaged.x;
        while (xpos < damaged.x + damaged.width) {
            int drawn_pixels = draw_x(xpos, ypos, items, len);
            xpos += drawn_pixels;
        }
    }
}

static void process_message(Context *ctx)
{
    MailboxMessage *mbox_msg = mailbox_take_message(&ctx->mailbox);
    Message *message = CONTAINER_OF(mbox_msg, Message, base);

    GenMessage gen_message;
    if (UNLIKELY(port_parse_gen_message(message->message, &gen_message) != GenCallMessage)) {
        goto invalid_message;
    }

    term req = gen_message.req;
    if (UNLIKELY(!term_is_tuple(req) || term_get_tuple_arity(req) < 1)) {
        goto invalid_message;
    }

    term cmd = term_get_tuple_element(req, 0);

    if (SDL_MUSTLOCK(surface)) {
        if (SDL_LockSurface(surface) < 0) {
            return;
        }
    }

    if (cmd == globalcontext_make_atom(ctx->global, "\x6"
                                      "update")) {
        term display_list = term_get_tuple_element(req, 1);
        do_update(ctx, display_list);
        prev_message = message;

        // Copy and scale up
        int scale = screen->scale;
        for (int ypos = 0; ypos < surface->h; ypos++) {
            for (int xpos = 0; xpos < surface->w; xpos++) {
                Uint32 *srcpix = (Uint32 *) (((uint8_t *) screen->pixels) + screen->w * (ypos / scale) * BPP + (xpos / scale) * BPP);
                Uint32 *destpix = (Uint32 *) (((uint8_t *) surface->pixels) + surface->w * ypos * BPP + xpos * BPP);
                *destpix = *srcpix;
            }
        }

    } else if (cmd == globalcontext_make_atom(ctx->global, "\xF"
                                             "subscribe_input")) {
        if (term_get_tuple_arity(req) != 2) {
            goto invalid_message;
        }
        term sources = term_get_tuple_element(req, 2);
        if (term_is_pid(keyboard_pid) || sources != globalcontext_make_atom(ctx->global, "\x3" "all")) {
            fprintf(stderr, "Warning: only one subscriber to all sources is supported now\n");
        }
        // TODO: selective subscribe
        keyboard_pid = gen_message.pid;

    } else if (cmd == globalcontext_make_atom(ctx->global, "\xA" "load_image")) {

        handle_load_image(req, gen_message.ref, gen_message.pid, ctx);

        goto free_msg_and_exit;

    } else if (cmd == globalcontext_make_atom(ctx->global, "\xD" "register_font")) {
        term font_bin = term_get_tuple_element(req, 2);
        EpdFont *loaded_font = ufont_parse(term_binary_data(font_bin), term_binary_size(font_bin));

        AtomString handle_atom = globalcontext_atomstring_from_term(ctx->global, term_get_tuple_element(req, 1));
        char handle[255];
        atom_string_to_c(handle_atom, handle, sizeof(handle));
        ufont_manager_register(ufont_manager, handle, loaded_font);

    } else {
        fprintf(stderr, "unexpected command: ");
        term_display(stderr, req, ctx);
        fprintf(stderr, "\n");
    }

    if (SDL_MUSTLOCK(surface)) {
        SDL_UnlockSurface(surface);
    }

    SDL_Flip(surface);

    if (UNLIKELY(memory_ensure_free(ctx, TUPLE_SIZE(3)) != MEMORY_GC_OK)) {
        abort();
    }
    term return_tuple = term_alloc_tuple(2, &ctx->heap);
    term_put_tuple_element(return_tuple, 0, gen_message.ref);
    term_put_tuple_element(return_tuple, 1, OK_ATOM);

    int local_process_id = term_to_local_process_id(gen_message.pid);
    globalcontext_send_message(ctx->global, local_process_id, return_tuple);

    goto free_msg_and_exit;

invalid_message:
    fprintf(stderr, "Got invalid message: ");
    term_display(stderr, message->message, ctx);
    fprintf(stderr, "\n");
    fprintf(stderr, "Expected gen_server call.\n");

free_msg_and_exit:
    if (prev_message != message) {
        destroy_message(message, ctx->global);
    }
    return;
}

static NativeHandlerResult consume_display_mailbox(Context *ctx)
{
    process_message(ctx);

    return NativeContinue;
}

static void send_message(term pid, term message, GlobalContext *global)
{
    int local_process_id = term_to_local_process_id(pid);
    globalcontext_send_message(global, local_process_id, message);
}

static inline int replace_new_line(int c)
{
    return c == '\r' ? '\n' : c;
}

void send_keyboard_event(struct KeyboardEvent *keyb, Context *ctx)
{
    GlobalContext *glb = ctx->global;

    if (keyboard_pid) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);

        avm_int_t millis = (ts.tv_sec - ts0.tv_sec) * 1000 + (ts.tv_nsec - ts0.tv_nsec) / 1000000;

        BEGIN_WITH_STACK_HEAP(TUPLE_SIZE(3) + TUPLE_SIZE(4), heap);

        term up_down = keyb->key_down ? globalcontext_make_atom(glb, ATOM_STR("\x4", "down"))
                                      : globalcontext_make_atom(glb, ATOM_STR("\x2", "up"));
        bool supress_key = false;
        term code_or_special;
        // unicode is valid only during key down
        if (keyb->unicode) {
            code_or_special = term_from_int(replace_new_line(keyb->unicode));
        } else {
            switch (keyb->key) {
                case 274:
                    code_or_special = globalcontext_make_atom(glb, ATOM_STR("\x4", "down"));
                    break;
                case 276:
                    code_or_special = globalcontext_make_atom(glb, ATOM_STR("\x4", "left"));
                    break;
                case 273:
                    code_or_special = globalcontext_make_atom(glb, ATOM_STR("\x2", "up"));
                    break;
                case 275:
                    code_or_special = globalcontext_make_atom(glb, ATOM_STR("\x5", "right"));
                    break;
                case 301:
                    code_or_special = globalcontext_make_atom(glb, ATOM_STR("\x9", "caps_lock"));
                    break;
                case 303:
                    code_or_special = globalcontext_make_atom(glb, ATOM_STR("\xB", "right_shift"));
                    break;
                case 304:
                    code_or_special = globalcontext_make_atom(glb, ATOM_STR("\x5", "shift"));
                    break;
                case 306:
                    code_or_special = globalcontext_make_atom(glb, ATOM_STR("\x4", "ctrl"));
                    break;
                case 308:
                    code_or_special = globalcontext_make_atom(glb, ATOM_STR("\x3", "alt"));
                    break;
                case 313:
                    code_or_special = globalcontext_make_atom(glb, ATOM_STR("\x5", "altgr"));
                    break;
                default:
                    if (keyb->key <= 127) {
                        code_or_special = term_from_int(replace_new_line(keyb->key));
                    } else {
                        fprintf(stderr, "Ignoring key: %i\n", (int) keyb->key);
                        supress_key = true;
                    }
            }
        }

        if (supress_key) {
            return;
        }

        term event_data_tuple = term_alloc_tuple(3, &heap);
        term_put_tuple_element(event_data_tuple, 0, globalcontext_make_atom(glb, "\x8"
                                                                           "keyboard"));
        term_put_tuple_element(event_data_tuple, 1, up_down);
        term_put_tuple_element(event_data_tuple, 2, code_or_special);

        term event_tuple = term_alloc_tuple(4, &heap);
        term_put_tuple_element(event_tuple, 0, globalcontext_make_atom(glb, "\xB"
                                                                      "input_event"));
        term_put_tuple_element(event_tuple, 1, term_from_local_process_id(ctx->process_id));
        term_put_tuple_element(event_tuple, 2, term_from_int(millis));
        term_put_tuple_element(event_tuple, 3, event_data_tuple);

        send_message(keyboard_pid, event_tuple, glb);

        END_WITH_STACK_HEAP(heap, glb);
    }
}

void send_mouse_event(struct MouseEvent *mouse, Context *ctx)
{
    GlobalContext *glb = ctx->global;

    if (keyboard_pid) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);

        avm_int_t millis = (ts.tv_sec - ts0.tv_sec) * 1000 + (ts.tv_nsec - ts0.tv_nsec) / 1000000;

        term released = globalcontext_make_atom(glb, ATOM_STR("\x8", "released"));
        term pressed = globalcontext_make_atom(glb, ATOM_STR("\x7", "pressed"));

        bool has_state_tuple = false;
        term event_type;
        switch (mouse->type) {
            case SDL_MOUSEMOTION:
                has_state_tuple = true;
                event_type = globalcontext_make_atom(glb, ATOM_STR("\x4", "move"));
                break;
            case SDL_MOUSEBUTTONDOWN:
                event_type = pressed;
                break;
            case SDL_MOUSEBUTTONUP:
                event_type = released;
                break;
            default:
                fprintf(stderr, "Unexpected mouse event type.\n");
                return;
        };

        BEGIN_WITH_STACK_HEAP(TUPLE_SIZE(3) + TUPLE_SIZE(5) + TUPLE_SIZE(4), heap);

        term state;
        if (has_state_tuple) {
            state = term_alloc_tuple(3, &heap);
            term_put_tuple_element(state, 0, (mouse->button & SDL_BUTTON(1)) ? pressed : released);
            term_put_tuple_element(state, 1, (mouse->button & SDL_BUTTON(2)) ? pressed : released);
            term_put_tuple_element(state, 2, (mouse->button & SDL_BUTTON(3)) ? pressed : released);
        } else {
            switch (mouse->button) {
                case SDL_BUTTON_LEFT:
                    state = globalcontext_make_atom(glb, ATOM_STR("\x4", "left"));
                    break;
                case SDL_BUTTON_MIDDLE:
                    state = globalcontext_make_atom(glb, ATOM_STR("\x6", "middle"));
                    break;
                case SDL_BUTTON_RIGHT:
                    state = globalcontext_make_atom(glb, ATOM_STR("\x5", "right"));
                    break;
            }
        }

        term event_data_tuple = term_alloc_tuple(5, &heap);
        term_put_tuple_element(event_data_tuple, 0, globalcontext_make_atom(glb, ATOM_STR("\x5", "mouse")));
        term_put_tuple_element(event_data_tuple, 1, event_type);
        term_put_tuple_element(event_data_tuple, 2, state);
        term_put_tuple_element(event_data_tuple, 3, term_from_int(mouse->x));
        term_put_tuple_element(event_data_tuple, 4, term_from_int(mouse->y));

        term event_tuple = term_alloc_tuple(4, &heap);
        term_put_tuple_element(event_tuple, 0, globalcontext_make_atom(glb, ATOM_STR("\xB", "input_event")));
        term_put_tuple_element(event_tuple, 1, term_from_local_process_id(ctx->process_id));
        term_put_tuple_element(event_tuple, 2, term_from_int(millis));
        term_put_tuple_element(event_tuple, 3, event_data_tuple);

        send_message(keyboard_pid, event_tuple, glb);

        END_WITH_STACK_HEAP(heap, glb);
    }
}

Context *display_create_port(GlobalContext *global, term opts)
{
    Context *ctx = context_new(global);
    ctx->native_handler = consume_display_mailbox;

    term width_atom = globalcontext_make_atom(ctx->global, "\x5"
                                             "width");
    term height_atom = globalcontext_make_atom(ctx->global, "\x6"
                                              "height");

    term width_term = interop_proplist_get_value_default(opts, width_atom, term_from_int(SCREEN_WIDTH));
    term height_term = interop_proplist_get_value_default(opts, height_atom, term_from_int(SCREEN_HEIGHT));

    avm_int_t width = term_to_int(width_term);
    avm_int_t height = term_to_int(height_term);

    struct DisplayOpts *disp_opts = malloc(sizeof(struct DisplayOpts));
    if (IS_NULL_PTR(disp_opts)) {
        abort();
    }
    disp_opts->width = width;
    disp_opts->height = height;
    ctx->platform_data = disp_opts;

    UNUSED(opts);

    pthread_t thread_id;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_create(&thread_id, &attr, display_loop, disp_opts);

    pthread_mutex_lock(&ready_mutex);
    pthread_cond_wait(&ready, &ready_mutex);
    pthread_mutex_unlock(&ready_mutex);
    the_ctx = ctx;

    clock_gettime(CLOCK_MONOTONIC, &ts0);

    return ctx;
}

static int get_scale()
{
    int scale = 1;
    const char *scale_str = getenv("AVM_SDL_DISPLAY_SCALE");
    if (scale_str && (strlen(scale_str) > 0)) {
        char *first_invalid;
        scale = strtol(scale_str, &first_invalid, 10);
        if (*first_invalid != '\0') {
            scale = 1;
        }
    }

    return scale;
}

void *display_loop(void *args)
{
    struct DisplayOpts *disp_opts = (struct DisplayOpts *) args;
    int scale = get_scale();

    pthread_mutex_lock(&ready_mutex);

    UNUSED(args);

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        abort();
    }

    SDL_EnableUNICODE(1);

    if (!(surface = SDL_SetVideoMode(disp_opts->width * scale, disp_opts->height * scale, DEPTH, SDL_HWSURFACE))) {
        SDL_Quit();
        abort();
    }

    if (SDL_MUSTLOCK(surface)) {
        if (SDL_LockSurface(surface) < 0) {
            return NULL;
        }
    }

    screen = malloc(sizeof(struct Screen));
    screen->w = disp_opts->width;
    screen->h = disp_opts->height;
    screen->scale = scale;
    screen->pixels = malloc(disp_opts->width * disp_opts->height * BPP);
    screen->format = surface->format;

    memset(screen->pixels, 0x80, disp_opts->width * disp_opts->height * BPP);
    memset(surface->pixels, 0x80, disp_opts->width * scale * disp_opts->height * scale * BPP);

    ufont_manager = ufont_manager_new();

    if (SDL_MUSTLOCK(surface)) {
        SDL_UnlockSurface(surface);
    }

    SDL_Flip(surface);

    pthread_cond_signal(&ready);
    pthread_mutex_unlock(&ready_mutex);

    SDL_Event event;

    while (SDL_WaitEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT: {
                exit(EXIT_SUCCESS);
                break;
            }

            case SDL_KEYDOWN: {
                struct KeyboardEvent keyb_event;
                memset(&keyb_event, 0, sizeof(struct KeyboardEvent));
                keyb_event.key = event.key.keysym.sym;
                keyb_event.unicode = event.key.keysym.unicode;
                keyb_event.key_down = true;
                send_keyboard_event(&keyb_event, the_ctx);
                break;
            }

            case SDL_KEYUP: {
                struct KeyboardEvent keyb_event;
                memset(&keyb_event, 0, sizeof(struct KeyboardEvent));
                keyb_event.key = event.key.keysym.sym;
                keyb_event.unicode = event.key.keysym.unicode;
                keyb_event.key_down = false;
                send_keyboard_event(&keyb_event, the_ctx);
                break;
            }

            case SDL_MOUSEMOTION: {
                struct MouseEvent mouse_event;
                memset(&mouse_event, 0, sizeof(struct MouseEvent));
                mouse_event.type = event.motion.type;
                mouse_event.button = event.motion.state;
                mouse_event.x = event.motion.x / scale;
                mouse_event.y = event.motion.y / scale;
                send_mouse_event(&mouse_event, the_ctx);
                break;
            }

            case SDL_MOUSEBUTTONDOWN: {
                struct MouseEvent mouse_event;
                memset(&mouse_event, 0, sizeof(struct MouseEvent));
                mouse_event.type = event.button.type;
                mouse_event.button = event.button.button;
                mouse_event.x = event.button.x / scale;
                mouse_event.y = event.button.y / scale;
                send_mouse_event(&mouse_event, the_ctx);
                break;
            }

            case SDL_MOUSEBUTTONUP: {
                struct MouseEvent mouse_event;
                memset(&mouse_event, 0, sizeof(struct MouseEvent));
                mouse_event.type = event.button.type;
                mouse_event.button = event.button.button;
                mouse_event.x = event.button.x / scale;
                mouse_event.y = event.button.y / scale;
                send_mouse_event(&mouse_event, the_ctx);
                break;
            }

            default: {
                break;
            }
        }
    }

    return NULL;
}
