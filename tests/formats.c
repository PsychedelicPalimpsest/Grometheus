#include <assert.h>
#include <string.h>

#include "internal.h"
int main(void) {
    struct fb_fix_screeninfo f = {.type = FB_TYPE_PACKED_PIXELS,
                                  .visual = FB_VISUAL_TRUECOLOR,
                                  .line_length = 16,
                                  .smem_len = 64};
    struct fb_var_screeninfo v = {.xres = 2,
                                  .yres = 1,
                                  .xres_virtual = 4,
                                  .yres_virtual = 4,
                                  .xoffset = 1,
                                  .yoffset = 1,
                                  .bits_per_pixel = 16,
                                  .red = {11, 5, 0},
                                  .green = {5, 6, 0},
                                  .blue = {0, 5, 0}};
    uint32_t pixels[] = {0xff0000, 0x00ff00};
    mui_surface s = {2, 1, 8, pixels};
    unsigned char buf[64];
    memset(buf, 0x5a, sizeof(buf));
    assert(!mui_fb_validate(&f, &v));
    mui_fb_copy(&s, buf, &f, &v);
    uint16_t red, green;
    memcpy(&red, buf + 18, 2);
    memcpy(&green, buf + 20, 2);
    assert(red == 0xf800 && green == 0x07e0 && buf[17] == 0x5a && buf[22] == 0x5a);
    v.red.offset = 5;
    assert(mui_fb_validate(&f, &v) < 0);
    v.red.offset = 11;
    v.yoffset = UINT32_MAX;
    assert(mui_fb_validate(&f, &v) < 0);
    v.yoffset = 1;
    v.bits_per_pixel = 24;
    v.red = (struct fb_bitfield){0, 8, 0};
    v.green = (struct fb_bitfield){8, 8, 0};
    v.blue = (struct fb_bitfield){16, 8, 0};
    memset(buf, 0x5a, sizeof(buf));
    assert(!mui_fb_validate(&f, &v));
    mui_fb_copy(&s, buf, &f, &v);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    assert(buf[19] == 255 && buf[20] == 0 && buf[21] == 0 && buf[22] == 0 && buf[23] == 255 &&
           buf[24] == 0);
#endif
    assert(buf[18] == 0x5a && buf[25] == 0x5a);
    v.bits_per_pixel = 32;
    v.red.offset = 16;
    v.blue.offset = 0;
    v.transp = (struct fb_bitfield){24, 8, 0};
    memset(buf, 0x5a, sizeof(buf));
    assert(!mui_fb_validate(&f, &v));
    mui_fb_copy(&s, buf, &f, &v);
    uint32_t argb;
    memcpy(&argb, buf + 20, 4);
    assert(argb == 0xffff0000);
    assert(buf[19] == 0x5a && buf[28] == 0x5a);
    /* Native copies must respect source stride, destination offsets and padding. */
    uint32_t padded[] = {0x00123456, 0x00abcdef, 0xdeadbeef, 0x00010203, 0x00fedcba, 0xdeadbeef};
    mui_surface rows = {2, 2, 12, padded};
    v.transp.length = 0;
    v.yres = 2;
    memset(buf, 0x5a, sizeof(buf));
    mui_fb_copy(&rows, buf, &f, &v);
    assert(!memcmp(buf + 20, padded, 8));
    assert(!memcmp(buf + 36, padded + 3, 8));
    assert(buf[19] == 0x5a && buf[28] == 0x5a && buf[35] == 0x5a && buf[44] == 0x5a);
    v.transp.length = 8;
    padded[0] = 0x12123456;
    mui_fb_copy(&rows, buf, &f, &v);
    memcpy(&argb, buf + 20, 4);
    assert(argb == 0xff123456);
    memcpy(&argb, buf + 36, 4);
    assert(argb == 0xff010203);
    v.yres = 1;
    v.red.msb_right = 1;
    assert(mui_fb_validate(&f, &v) < 0);
    v.red.msb_right = 0;
    f.smem_len = 31;
    assert(mui_fb_validate(&f, &v) < 0);
    f.smem_len = 64;
    v.transp.offset = 0;
    assert(mui_fb_validate(&f, &v) < 0);
    mui_display *d = (void *)1;
    mui_options o = {MUI_AUTO, "/dev/null"};
    assert(mui_open(&d, &o) < 0 && !d);
    o.backend = MUI_DRM;
    assert(mui_open(&d, &o) < 0 && !d);
    assert(!mui_close(NULL));
    return 0;
}
