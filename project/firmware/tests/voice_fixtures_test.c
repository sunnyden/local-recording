#include "recorder_core.h"
#include "cJSON.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static unsigned nibble(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    assert(!"Invalid hexadecimal fixture");
    return 0;
}
static uint64_t integer(cJSON *json, const char *name)
{
    cJSON *field = cJSON_GetObjectItemCaseSensitive(json, name);
    assert(cJSON_IsNumber(field) && field->valuedouble >= 0 &&
           field->valuedouble <= 9007199254740991.0);
    uint64_t result = (uint64_t)field->valuedouble;
    assert((double)result == field->valuedouble);
    return result;
}
static void fixture(cJSON *json, bool valid)
{
    cJSON *hex = cJSON_GetObjectItemCaseSensitive(json, "hex");
    assert(cJSON_IsString(hex));
    size_t chars = strlen(hex->valuestring);
    assert(!(chars & 1) && chars <= 16384);
    size_t size = chars / 2;
    uint8_t *bytes = malloc(size ? size : 1);
    assert(bytes);
    for (size_t i = 0; i < size; ++i)
        bytes[i] = (uint8_t)((nibble(hex->valuestring[i * 2]) << 4) |
                            nibble(hex->valuestring[i * 2 + 1]));
    voice_frame_t frame;
    assert(voice_decode(bytes, size, &frame) == valid);
    if (valid) {
        assert(frame.kind == integer(json, "kind"));
        assert(frame.epoch == integer(json, "epoch"));
        assert(frame.sequence == integer(json, "sequence"));
        assert(frame.sample == integer(json, "sample_position"));
        cJSON *samples = cJSON_GetObjectItemCaseSensitive(json, "samples");
        assert(cJSON_IsArray(samples) && cJSON_GetArraySize(samples) == (int)frame.samples);
        for (size_t i = 0; i < frame.samples; ++i) {
            int value = frame.pcm[i * 2] | ((int)frame.pcm[i * 2 + 1] << 8);
            if (value >= 32768) value -= 65536;
            cJSON *sample = cJSON_GetArrayItem(samples, (int)i);
            assert(cJSON_IsNumber(sample) && sample->valuedouble == value);
        }
        uint8_t encoded[VOICE_MAX_PACKET];
        assert(voice_encode(encoded, sizeof(encoded), &frame));
        assert(!memcmp(encoded, bytes, size));
    }
    free(bytes);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    assert(file && !fseek(file, 0, SEEK_END));
    long size = ftell(file);
    assert(size > 0 && size <= 65536 && !fseek(file, 0, SEEK_SET));
    char *source = calloc(1, (size_t)size + 1);
    assert(source && fread(source, 1, (size_t)size, file) == (size_t)size);
    fclose(file);
    cJSON *json = cJSON_ParseWithLength(source, (size_t)size);
    assert(json && integer(json, "version") == 1);
    cJSON *valid = cJSON_GetObjectItemCaseSensitive(json, "valid");
    cJSON *invalid = cJSON_GetObjectItemCaseSensitive(json, "invalid");
    assert(cJSON_IsArray(valid) && cJSON_IsArray(invalid));
    int valid_count = cJSON_GetArraySize(valid), invalid_count = cJSON_GetArraySize(invalid);
    assert(valid_count > 0 && invalid_count > 0);
    for (int i = 0; i < valid_count; ++i) fixture(cJSON_GetArrayItem(valid, i), true);
    for (int i = 0; i < invalid_count; ++i) fixture(cJSON_GetArrayItem(invalid, i), false);
    cJSON_Delete(json); free(source);
    printf("PASS: shared voice-v1 fixtures (%d valid, %d invalid), exact encoder bytes and signed samples\n",
           valid_count, invalid_count);
    return 0;
}
