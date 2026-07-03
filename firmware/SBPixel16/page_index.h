#pragma once

// GET /api/status  — live runtime stats
void get_status(AsyncWebServerRequest *request) {
    char buf[192];
    snprintf(buf, sizeof(buf),
        "{\"fps\":%lu,\"e131Packets\":%lu,\"ddpPackets\":%lu"
        ",\"protocol\":\"%s\""
        ",\"vin1_mv\":%lu,\"vin2_mv\":%lu"
        ",\"testMode\":%u}",
        (unsigned long)g_fps,
        (unsigned long)g_e131Packets,
        (unsigned long)g_ddpPackets,
        cfg.protocol == PROTO_DDP ? "ddp" : "e131",
        (unsigned long)g_vin1_mv,
        (unsigned long)g_vin2_mv,
        g_testMode);
    request->send(200, "application/json", buf);
}
