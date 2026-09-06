#include "recorder_core.h"
#include "http_limits.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void wav_tests(void)
{
    uint8_t header[44];
    assert(wav_header(header, 640));
    assert(!wav_header(header, 3));
    assert(!wav_header(header, UINT32_MAX));
    FILE *f = fopen("core-test.wav", "wb+");
    assert(f);
    assert(wav_header(header, 640));
    assert(fwrite(header, 1, 44, f) == 44);
    uint8_t pcm[641] = {0};
    assert(fwrite(pcm, 1, 640, f) == 640);
    assert(!fflush(f));
    wav_info_t info;
    assert(wav_parse(f, &info));
    assert(info.offset == 44 && info.bytes == 640);
    assert(!wav_repair_limit(f, 642));
    assert(!wav_repair_limit(f, 639));
    assert(!fseek(f, 0, SEEK_SET));
    wav_header(header, 0);
    assert(fwrite(header, 1, 44, f) == 44);
    assert(!fseek(f, 0, SEEK_END));
    assert(fwrite(pcm, 1, 1, f) == 1);
    assert(wav_repair(f));
    assert(wav_parse(f, &info) && info.bytes == 640);
    assert(!fseek(f, 22, SEEK_SET));
    assert(fputc(2, f) == 2);
    assert(!fflush(f));
    assert(!wav_parse(f, &info));
    assert(!wav_repair(f));
    fclose(f);
    remove("core-test.wav");
    uint8_t checkpoint[16];
    uint32_t bytes = 0;
    recording_checkpoint_encode(checkpoint, 640);
    assert(recording_checkpoint_decode(checkpoint, &bytes) && bytes == 640);
    for (unsigned i = 0; i < 16; ++i) {
        checkpoint[i] ^= 1;
        assert(!recording_checkpoint_decode(checkpoint, &bytes));
        checkpoint[i] ^= 1;
    }
    f = fopen("core-test.wav", "wb+");
    assert(f);
    wav_header(header, 640);
    header[4] = (640 + 48) & 255;
    header[5] = (640 + 48) >> 8;
    assert(fwrite(header, 1, 36, f) == 36);
    const uint8_t junk[] = {'J','U','N','K',3,0,0,0,1,2,3,0};
    assert(fwrite(junk, 1, sizeof(junk), f) == sizeof(junk));
    assert(fwrite(header + 36, 1, 8, f) == 8);
    assert(fwrite(pcm, 1, 640, f) == 640);
    assert(!fflush(f));
    assert(wav_parse(f, &info) && info.offset == 56 && info.bytes == 640);
    assert(!wav_repair(f));
    fclose(f);
    remove("core-test.wav");
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
    wav_tests();
    voice_tests();
    http_buffer_tests();
    puts("PASS: WAV creation/parser/recovery, voice framing/alignment, upload ranges");
    return 0;
}
