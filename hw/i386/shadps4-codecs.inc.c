/* shadPS4 guest-side codec helpers. */

static void shadps4_png_chunk(GByteArray *png, const char type[4],
                              const uint8_t *data, uint32_t size)
{
    uint8_t word[4];
    uLong crc;

    stl_be_p(word, size);
    g_byte_array_append(png, word, 4);
    g_byte_array_append(png, (const uint8_t *)type, 4);
    if (size) {
        g_byte_array_append(png, data, size);
    }
    crc = crc32(0, (const Bytef *)type, 4);
    crc = crc32(crc, data, size);
    stl_be_p(word, crc);
    g_byte_array_append(png, word, 4);
}

static uint64_t shadps4_hle_png_encode(CPUState *cs, uint64_t param_addr,
                                       uint64_t output_info_addr)
{
    static const uint8_t signature[8] = {
        0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'
    };
    uint8_t param[48], ihdr[13] = { 0 };
    g_autoptr(GByteArray) png = g_byte_array_new();
    g_autofree uint8_t *input = NULL;
    g_autofree uint8_t *raw = NULL;
    g_autofree uint8_t *compressed = NULL;
    uint64_t input_addr, output_addr, input_required, raw_size;
    uint32_t input_size, output_size, width, height, pitch, channels, y;
    uint16_t pixel_format, color_space, bit_depth, level;
    uLongf compressed_size;

    if (!param_addr ||
        !shadps4_guest_rw(cs, param_addr, param, sizeof(param), false)) {
        return UINT32_C(0x80690103);
    }
    input_addr = ldq_le_p(param);
    output_addr = ldq_le_p(param + 8);
    input_size = ldl_le_p(param + 16);
    output_size = ldl_le_p(param + 20);
    width = ldl_le_p(param + 24);
    height = ldl_le_p(param + 28);
    pitch = ldl_le_p(param + 32);
    pixel_format = lduw_le_p(param + 36);
    color_space = lduw_le_p(param + 38);
    bit_depth = lduw_le_p(param + 40);
    level = lduw_le_p(param + 46);
    if (!input_addr || !output_addr) {
        return UINT32_C(0x80690101);
    }
    if (!width || !height || width > 16384 || height > 16384 ||
        bit_depth != 8 || pixel_format > 1 ||
        (color_space != 3 && color_space != 19)) {
        return UINT32_C(0x80690102);
    }
    pitch = pitch ? pitch : width * 4;
    channels = color_space == 19 ? 4 : 3;
    input_required = (uint64_t)pitch * height;
    raw_size = ((uint64_t)width * channels + 1) * height;
    if (pitch < width * 4 || input_required > input_size ||
        input_required > 128 * MiB || raw_size > 128 * MiB) {
        return UINT32_C(0x80690102);
    }
    input = g_malloc(input_required);
    raw = g_malloc(raw_size);
    if (!shadps4_guest_rw(cs, input_addr, input, input_required, false)) {
        return UINT32_C(0x80690101);
    }
    for (y = 0; y < height; y++) {
        const uint8_t *src = input + (uint64_t)y * pitch;
        uint8_t *dst = raw + (uint64_t)y * (width * channels + 1);
        uint32_t x;

        *dst++ = 0;
        for (x = 0; x < width; x++, src += 4) {
            *dst++ = src[pixel_format ? 2 : 0];
            *dst++ = src[1];
            *dst++ = src[pixel_format ? 0 : 2];
            if (channels == 4) {
                *dst++ = src[3];
            }
        }
    }
    compressed_size = compressBound(raw_size);
    compressed = g_malloc(compressed_size);
    if (compress2(compressed, &compressed_size, raw, raw_size,
                  MIN(level, 9)) != Z_OK) {
        return UINT32_C(0x80690120);
    }
    g_byte_array_append(png, signature, 8);
    stl_be_p(ihdr, width);
    stl_be_p(ihdr + 4, height);
    ihdr[8] = 8;
    ihdr[9] = channels == 4 ? 6 : 2;
    shadps4_png_chunk(png, "IHDR", ihdr, 13);
    shadps4_png_chunk(png, "IDAT", compressed, compressed_size);
    shadps4_png_chunk(png, "IEND", NULL, 0);
    if (png->len > output_size) {
        return UINT32_C(0x80690110);
    }
    if (!shadps4_guest_rw(cs, output_addr, png->data, png->len, true)) {
        return UINT32_C(0x80690101);
    }
    if (output_info_addr) {
        uint32_t info[2] = { cpu_to_le32(png->len), cpu_to_le32(height) };

        if (!shadps4_guest_rw(cs, output_info_addr, info, sizeof(info), true)) {
            return UINT32_C(0x80690101);
        }
    }
    return png->len;
}
