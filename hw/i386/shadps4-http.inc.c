/* Portable guest-side HTTP parsing and URI helpers. */

#define SHADPS4_HTTP_ERROR_NOMEM UINT32_C(0x80431022)
#define SHADPS4_HTTP_ERROR_INVALID_VALUE UINT32_C(0x804311fe)
#define SHADPS4_HTTP_ERROR_INVALID_URL UINT32_C(0x80433060)
#define SHADPS4_HTTP_ERROR_PARSE_NOT_FOUND UINT32_C(0x80432025)
#define SHADPS4_HTTP_ERROR_PARSE_RESPONSE UINT32_C(0x80432060)
#define SHADPS4_HTTP_ERROR_PARSE_VALUE UINT32_C(0x804321fe)

static uint64_t shadps4_http_write_string_result(CPUState *cs,
                                                 uint64_t output,
                                                 uint64_t required,
                                                 uint64_t capacity,
                                                 const char *value)
{
    uint64_t size = strlen(value) + 1;

    if (required && shadps4_hle_write_u64(cs, required, size)) {
        return SHADPS4_HTTP_ERROR_INVALID_VALUE;
    }
    if (!output) {
        return 0;
    }
    if (capacity < size) {
        return SHADPS4_HTTP_ERROR_NOMEM;
    }
    return shadps4_guest_rw(cs, output, (void *)value, size, true) ?
           0 : SHADPS4_HTTP_ERROR_INVALID_VALUE;
}

static uint64_t shadps4_http_parse_status(CPUState *cs, uint64_t address,
                                          uint64_t length,
                                          uint64_t major_address,
                                          uint64_t minor_address,
                                          uint64_t code_address,
                                          uint64_t phrase_address)
{
    g_autofree char *line = NULL;
    const char *p;
    const char *phrase;
    uint64_t phrase_length_address;
    uint64_t phrase_guest;
    uint64_t consumed;
    unsigned major, minor, code;

    if (!address || !length || length > 16 * KiB) {
        return SHADPS4_HTTP_ERROR_PARSE_RESPONSE;
    }
    if (!major_address || !minor_address || !code_address || !phrase_address ||
        !shadps4_hle_argument(cs, 6, &phrase_length_address) ||
        !phrase_length_address) {
        return SHADPS4_HTTP_ERROR_PARSE_VALUE;
    }
    line = g_malloc(length + 1);
    if (!shadps4_guest_rw(cs, address, line, length, false)) {
        return SHADPS4_HTTP_ERROR_PARSE_RESPONSE;
    }
    line[length] = 0;
    p = memchr(line, '\n', length);
    if (!p || sscanf(line, "HTTP/%u.%u %u", &major, &minor, &code) != 3 ||
        major > INT32_MAX || minor > INT32_MAX || code > 999) {
        return SHADPS4_HTTP_ERROR_PARSE_RESPONSE;
    }
    phrase = strchr(line, ' ');
    phrase = phrase ? strchr(phrase + 1, ' ') : NULL;
    if (!phrase) {
        return SHADPS4_HTTP_ERROR_PARSE_RESPONSE;
    }
    phrase++;
    consumed = p - line + 1;
    phrase_guest = address + (phrase - line);
    length = p - phrase;
    if (length && phrase[length - 1] == '\r') {
        length--;
    }
    return shadps4_hle_write_u32(cs, major_address, major) ||
           shadps4_hle_write_u32(cs, minor_address, minor) ||
           shadps4_hle_write_u32(cs, code_address, code) ||
           shadps4_hle_write_u64(cs, phrase_address, phrase_guest) ||
           shadps4_hle_write_u64(cs, phrase_length_address, length) ?
           SHADPS4_HTTP_ERROR_PARSE_VALUE : consumed;
}

static uint64_t shadps4_http_parse_header(CPUState *cs, uint64_t address,
                                          uint64_t length,
                                          uint64_t field_address,
                                          uint64_t value_address,
                                          uint64_t length_address)
{
    g_autofree char *header = NULL;
    char field[256];
    char *line;
    size_t field_length;

    if (!address || !length || length > 2 * MiB) {
        return SHADPS4_HTTP_ERROR_PARSE_RESPONSE;
    }
    if (!value_address || !length_address ||
        !shadps4_guest_read_string(cs, field_address, field, sizeof(field))) {
        return SHADPS4_HTTP_ERROR_PARSE_VALUE;
    }
    header = g_malloc(length + 1);
    if (!shadps4_guest_rw(cs, address, header, length, false)) {
        return SHADPS4_HTTP_ERROR_PARSE_RESPONSE;
    }
    header[length] = 0;
    field_length = strlen(field);
    for (line = header; line < header + length;) {
        char *end = memchr(line, '\n', header + length - line);
        char *colon = memchr(line, ':', end ? end - line : header + length - line);

        if (!end) {
            end = header + length;
        }
        if (colon && colon - line == field_length &&
            !g_ascii_strncasecmp(line, field, field_length)) {
            char *value = colon + 1;
            size_t value_length;
            uint64_t guest_value;

            while (value < end && (*value == ' ' || *value == '\t')) {
                value++;
            }
            value_length = end - value;
            if (value_length && value[value_length - 1] == '\r') {
                value_length--;
            }
            guest_value = value_length ? address + (value - header) : 0;
            return shadps4_hle_write_u64(cs, value_address, guest_value) ||
                   shadps4_hle_write_u64(cs, length_address, value_length) ?
                   SHADPS4_HTTP_ERROR_PARSE_VALUE : end - header +
                   (end < header + length);
        }
        line = end + (end < header + length);
    }
    return SHADPS4_HTTP_ERROR_PARSE_NOT_FOUND;
}

static bool shadps4_http_uri_read_element(CPUState *cs, uint64_t address,
                                          char fields[7][4096],
                                          bool *opaque, uint16_t *port)
{
    uint8_t element[80];
    uint32_t i;

    if (!address || !shadps4_guest_rw(cs, address, element,
                                      sizeof(element), false)) {
        return false;
    }
    *opaque = element[0];
    *port = lduw_le_p(element + 64);
    for (i = 0; i < 7; i++) {
        uint64_t pointer = ldq_le_p(element + 8 + i * 8);

        fields[i][0] = 0;
        if (pointer && !shadps4_guest_read_string(cs, pointer, fields[i],
                                                  sizeof(fields[i]))) {
            return false;
        }
    }
    return true;
}

static uint64_t shadps4_http_uri_build(CPUState *cs, uint64_t output,
                                       uint64_t required, uint64_t capacity,
                                       uint64_t element_address,
                                       uint32_t options)
{
    char fields[7][4096];
    g_autoptr(GString) uri = g_string_new(NULL);
    bool opaque;
    uint16_t port;

    if (!shadps4_http_uri_read_element(cs, element_address, fields,
                                       &opaque, &port)) {
        return SHADPS4_HTTP_ERROR_INVALID_URL;
    }
    if ((options & 1) && fields[0][0]) {
        g_string_append_printf(uri, "%s:", fields[0]);
    }
    if (!opaque) {
        g_string_append(uri, "//");
    }
    if ((options & 0x10) && fields[1][0]) {
        g_string_append(uri, fields[1]);
        if ((options & 0x20) && fields[2][0]) {
            g_string_append_printf(uri, ":%s", fields[2]);
        }
        g_string_append_c(uri, '@');
    }
    if (options & 2) {
        g_string_append(uri, fields[3]);
    }
    if ((options & 4) && port &&
        !((!g_ascii_strcasecmp(fields[0], "http") && port == 80) ||
          (!g_ascii_strcasecmp(fields[0], "https") && port == 443))) {
        g_string_append_printf(uri, ":%u", port);
    }
    if (options & 8) {
        g_string_append(uri, fields[4]);
    }
    if ((options & 0x40) && fields[5][0]) {
        g_string_append(uri, fields[5][0] == '?' ? fields[5] : "?");
        if (fields[5][0] != '?') {
            g_string_append(uri, fields[5]);
        }
    }
    if ((options & 0x80) && fields[6][0]) {
        g_string_append(uri, fields[6][0] == '#' ? fields[6] : "#");
        if (fields[6][0] != '#') {
            g_string_append(uri, fields[6]);
        }
    }
    return shadps4_http_write_string_result(cs, output, required, capacity,
                                            uri->str);
}

static uint64_t shadps4_http_uri_transform(CPUState *cs, uint64_t output,
                                           uint64_t required,
                                           uint64_t capacity,
                                           uint64_t input_address,
                                           bool escape)
{
    char input[16384];
    g_autofree char *value = NULL;

    if (!shadps4_guest_read_string(cs, input_address, input, sizeof(input))) {
        return SHADPS4_HTTP_ERROR_INVALID_VALUE;
    }
    value = escape ? g_uri_escape_string(input, NULL, false) :
                     g_uri_unescape_string(input, NULL);
    if (!value) {
        return SHADPS4_HTTP_ERROR_INVALID_VALUE;
    }
    return shadps4_http_write_string_result(cs, output, required, capacity,
                                            value);
}

static uint64_t shadps4_http_uri_sweep(CPUState *cs, uint64_t output,
                                       uint64_t input_address,
                                       uint64_t input_size)
{
    g_autofree char *input = NULL;
    g_auto(GStrv) parts = NULL;
    g_autoptr(GPtrArray) stack = NULL;
    g_autoptr(GString) result = NULL;
    uint32_t i;

    if (!input_size) {
        return 0;
    }
    if (!output || !input_address || input_size > 16 * KiB) {
        return SHADPS4_HTTP_ERROR_INVALID_VALUE;
    }
    input = g_malloc(input_size + 1);
    if (!shadps4_guest_rw(cs, input_address, input, input_size, false)) {
        return SHADPS4_HTTP_ERROR_INVALID_VALUE;
    }
    input[input_size] = 0;
    parts = g_strsplit(input, "/", -1);
    stack = g_ptr_array_new();
    for (i = 0; parts[i]; i++) {
        if (!strcmp(parts[i], "..")) {
            if (stack->len) {
                g_ptr_array_remove_index(stack, stack->len - 1);
            }
        } else if (parts[i][0] && strcmp(parts[i], ".")) {
            g_ptr_array_add(stack, parts[i]);
        }
    }
    result = g_string_new(input[0] == '/' ? "/" : "");
    for (i = 0; i < stack->len; i++) {
        if (result->len && result->str[result->len - 1] != '/') {
            g_string_append_c(result, '/');
        }
        g_string_append(result, g_ptr_array_index(stack, i));
    }
    return shadps4_guest_rw(cs, output, result->str, result->len + 1, true) ?
           0 : SHADPS4_HTTP_ERROR_INVALID_VALUE;
}

static uint64_t shadps4_http_uri_parse(CPUState *cs, uint64_t output,
                                       uint64_t input_address,
                                       uint64_t pool_address,
                                       uint64_t required,
                                       uint64_t capacity)
{
    char input[16384];
    g_autoptr(GUri) uri = NULL;
    GError *error = NULL;
    const char *values[7];
    uint8_t element[80] = { 0 };
    uint64_t needed = 0;
    uint32_t i;

    if (!shadps4_guest_read_string(cs, input_address, input, sizeof(input))) {
        return SHADPS4_HTTP_ERROR_INVALID_URL;
    }
    uri = g_uri_parse(input, G_URI_FLAGS_PARSE_RELAXED, &error);
    if (!uri) {
        g_clear_error(&error);
        return SHADPS4_HTTP_ERROR_INVALID_URL;
    }
    values[0] = g_uri_get_scheme(uri) ?: "";
    values[1] = g_uri_get_user(uri) ?: "";
    values[2] = g_uri_get_password(uri) ?: "";
    values[3] = g_uri_get_host(uri) ?: "";
    values[4] = g_uri_get_path(uri) ?: "";
    values[5] = g_uri_get_query(uri) ?: "";
    values[6] = g_uri_get_fragment(uri) ?: "";
    for (i = 0; i < 7; i++) {
        needed += strlen(values[i]) + 1;
    }
    if (required && shadps4_hle_write_u64(cs, required, needed)) {
        return SHADPS4_HTTP_ERROR_INVALID_VALUE;
    }
    if (!output || !pool_address) {
        return required ? 0 : SHADPS4_HTTP_ERROR_INVALID_VALUE;
    }
    if (capacity < needed) {
        return SHADPS4_HTTP_ERROR_NOMEM;
    }
    element[0] = strstr(input, "//") == NULL;
    for (i = 0, needed = 0; i < 7; i++) {
        size_t size = strlen(values[i]) + 1;

        stq_le_p(element + 8 + i * 8, pool_address + needed);
        if (!shadps4_guest_rw(cs, pool_address + needed,
                              (void *)values[i], size, true)) {
            return SHADPS4_HTTP_ERROR_INVALID_VALUE;
        }
        needed += size;
    }
    stw_le_p(element + 64, g_uri_get_port(uri) > 0 ?
             g_uri_get_port(uri) :
             !g_ascii_strcasecmp(values[0], "https") ? 443 :
             !g_ascii_strcasecmp(values[0], "http") ? 80 : 0);
    return shadps4_guest_rw(cs, output, element, sizeof(element), true) ?
           0 : SHADPS4_HTTP_ERROR_INVALID_VALUE;
}

static uint64_t shadps4_http_uri_merge(CPUState *cs, uint64_t output,
                                       uint64_t base_address,
                                       uint64_t relative_address,
                                       uint64_t required,
                                       uint64_t capacity, uint32_t options)
{
    char base[16384], relative[16384];
    g_autofree char *merged = NULL;
    GError *error = NULL;

    if (options ||
        !shadps4_guest_read_string(cs, base_address, base, sizeof(base)) ||
        !shadps4_guest_read_string(cs, relative_address, relative,
                                   sizeof(relative))) {
        return SHADPS4_HTTP_ERROR_INVALID_VALUE;
    }
    merged = g_uri_resolve_relative(base, relative,
                                    G_URI_FLAGS_PARSE_RELAXED, &error);
    if (!merged) {
        g_clear_error(&error);
        return SHADPS4_HTTP_ERROR_INVALID_URL;
    }
    return shadps4_http_write_string_result(cs, output, required, capacity,
                                            merged);
}
