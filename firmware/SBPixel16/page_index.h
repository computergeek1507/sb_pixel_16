#pragma once

// GET /api/status  — live runtime stats
void get_status(AsyncWebServerRequest *request) {
    char buf[384];
    const char *proto = cfg.protocol == PROTO_DDP  ? "ddp"
                      : cfg.protocol == PROTO_FSEQ ? "fseq"
                                                   : "e131";
    snprintf(buf, sizeof(buf),
        "{\"fps\":%lu,\"e131Packets\":%lu,\"ddpPackets\":%lu"
        ",\"protocol\":\"%s\""
        ",\"vin1_mv\":%lu,\"vin2_mv\":%lu"
        ",\"testMode\":%u"
        ",\"sdMounted\":%s,\"sdSizeMB\":%lu,\"fseqCount\":%u"
        ",\"fseqName\":\"%s\",\"fseqFrame\":%lu,\"fseqFrames\":%lu}",
        (unsigned long)g_fps,
        (unsigned long)g_e131Packets,
        (unsigned long)g_ddpPackets,
        proto,
        (unsigned long)g_vin1_mv,
        (unsigned long)g_vin2_mv,
        g_testMode,
        g_sdMounted ? "true" : "false",
        (unsigned long)g_sdSizeMB,
        g_fseqCount,
        g_fseqName,
        (unsigned long)g_fseqFrame,
        (unsigned long)g_fseqFrames);
    request->send(200, "application/json", buf);
}
