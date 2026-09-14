#include "recorder_core.h"
#include "http_limits.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void ogg_opus_tests(void)
{
    FILE *f = fopen("core-test.opus", "wb+");
    assert(f);
    ogg_opus_writer_t writer;
    assert(ogg_opus_writer_begin(&writer, f, 0x12345678, 312));
    uint8_t packet[60] = {0x48};
    for (unsigned i = 0; i < 51; ++i)
        assert(ogg_opus_writer_packet(&writer, packet, sizeof(packet), PCM_SAMPLES));
    assert(!fflush(f));
    long safe = ftell(f);
    uint32_t page = writer.last_page_offset;
    assert(ogg_opus_writer_packet(&writer, packet, sizeof(packet), PCM_SAMPLES));
    assert(safe > 0 && page > 0 &&
           ogg_opus_writer_finish(&writer, 51 * PCM_SAMPLES));
    assert(!fflush(f));
    ogg_opus_info_t info;
    assert(ogg_opus_parse(f, 0, true, &info));
    assert(info.serial == 0x12345678 && info.samples == 51 * PCM_SAMPLES);
    assert(info.pre_skip == 312 && info.eos);
    assert(!fseek(f, 22, SEEK_SET));
    int byte = fgetc(f);
    assert(byte != EOF && !fseek(f, 22, SEEK_SET) && fputc(byte ^ 1, f) != EOF);
    assert(!fflush(f) && !ogg_opus_parse(f, 0, true, &info));
    fclose(f);

    f = fopen("core-repair.opus", "wb+");
    assert(f && ogg_opus_writer_begin(&writer, f, 7, 312));
    for (unsigned i = 0; i < 51; ++i)
        assert(ogg_opus_writer_packet(&writer, packet, sizeof(packet), PCM_SAMPLES));
    assert(!fflush(f));
    safe = ftell(f); page = writer.last_page_offset;
    assert(safe > 0 && ogg_opus_parse(f, (uint32_t)safe, false, &info));
    assert(!info.eos && info.samples == 50 * PCM_SAMPLES - 104);
    assert(ogg_opus_repair(f, (uint32_t)safe, page));
    assert(ogg_opus_parse(f, 0, true, &info) &&
           info.samples == 50 * PCM_SAMPLES - 104);
    fclose(f);
    remove("core-test.opus");
    remove("core-repair.opus");

    uint8_t checkpoint[24];
    uint32_t bytes = 0, decoded_page = 0;
    recording_checkpoint_encode(checkpoint, (uint32_t)safe, page);
    assert(recording_checkpoint_decode(checkpoint, &bytes, &decoded_page));
    assert(bytes == (uint32_t)safe && decoded_page == page);
    for (unsigned i = 0; i < sizeof(checkpoint); ++i) {
        checkpoint[i] ^= 1;
        assert(!recording_checkpoint_decode(checkpoint, &bytes, &decoded_page));
        checkpoint[i] ^= 1;
    }
}
static void voice_tests(void)
{
    uint8_t packet[VOICE_MAX_PACKET], pcm[PCM_BYTES] = {1, 2};
    voice_frame_t f = {.kind = 2, .epoch = 7, .pcm = pcm, .samples = PCM_SAMPLES}, out;
    assert(voice_encode(packet, sizeof(packet), &f));
    assert(voice_decode(packet, sizeof(packet), &out));
    assert(out.epoch == 7 && out.sequence == 0 && out.sample == 0);
    voice_frame_t expected = {.kind = 2, .epoch = 7};
    assert(voice_advance(&expected, &out));
    assert(!voice_advance(&expected, &out));
    assert(expected.sample == 320 && expected.sequence == 1);
    packet[6] = 1; assert(!voice_decode(packet, sizeof(packet), &out)); packet[6] = 0;
    packet[4] = 2; assert(!voice_decode(packet, sizeof(packet), &out)); packet[4] = 1;
    packet[0] = 0; assert(!voice_decode(packet, sizeof(packet), &out)); packet[0] = 'E';
    assert(!voice_decode(packet, 24, &out));
    assert(!voice_decode(packet, 25, &out));
    assert(!voice_decode(packet, 666, &out));
    for (unsigned i = 0; i < 4; ++i) packet[8 + i] = 0;
    assert(!voice_decode(packet, sizeof(packet), &out));
    packet[5] = 1; assert(voice_decode(packet, sizeof(packet), &out));
    packet[8] = 1; assert(!voice_decode(packet, sizeof(packet), &out));
    f.sequence = 0x12345678;
    f.sample = UINT64_C(0x123456789abcdef0);
    assert(voice_encode(packet, sizeof(packet), &f));
    assert(packet[12] == 0x78 && packet[15] == 0x12);
    assert(packet[16] == 0xf0 && packet[23] == 0x12);
    assert(voice_decode(packet, sizeof(packet), &out));
    assert(out.sample == f.sample && out.sequence == f.sequence);
    expected = out;
    expected.sequence = UINT32_MAX;
    out.sequence = UINT32_MAX;
    assert(!voice_advance(&expected, &out));
    expected = out;
    expected.sequence = out.sequence = 0;
    expected.sample = out.sample = UINT64_MAX;
    assert(!voice_advance(&expected, &out));
    assert(upload_range_bytes(700000, 0) == 327680);
    assert(upload_range_bytes(700000, 655360) == 44640);
    assert(upload_range_bytes(700000, 700000) == 0);
    assert(upload_range_bytes(700000, 700001) == 0);
}
static void http_buffer_tests(void)
{
    char token[RECORDER_HTTP_MAX_BEARER + 2];
    char url[RECORDER_HTTP_MAX_URL + 2];
    memset(token, 'A', sizeof(token));
    token[1467] = 0;
    const char *graph = "https://graph.microsoft.com/v1.0/me/drive?$select=id";
    assert(recorder_http_tx_size(graph, token) > 1551);
    token[1467] = 'A';
    token[RECORDER_HTTP_MAX_BEARER] = 0;
    memset(url, 'x', sizeof(url));
    url[RECORDER_HTTP_MAX_URL] = 0;
    assert(recorder_http_tx_size(url, token) == 12800);
    assert(recorder_http_tx_size(url, NULL) == (int)(RECORDER_HTTP_MAX_URL + 512u));
    url[RECORDER_HTTP_MAX_URL] = 'x';
    url[RECORDER_HTTP_MAX_URL + 1] = 0;
    assert(recorder_http_tx_size(url, NULL) == 0);
    token[RECORDER_HTTP_MAX_BEARER] = 'A';
    token[RECORDER_HTTP_MAX_BEARER + 1] = 0;
    assert(recorder_http_tx_size(graph, token) == 0);
    assert(recorder_http_tx_size(NULL, NULL) == 0);
    assert(recorder_http_tx_size("https://example.invalid", NULL) == 1024);
}

int main(void)
{
    ogg_opus_tests();
    voice_tests();
    http_buffer_tests();
    puts("PASS: Ogg Opus creation/parser/recovery, voice framing/alignment, upload ranges");
    return 0;
}
