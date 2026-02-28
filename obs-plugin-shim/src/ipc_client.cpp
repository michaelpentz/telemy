#include "ipc_client.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <chrono>
#include <algorithm>
#include <cstring>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <unordered_map>

namespace {
constexpr const char* kCmdPipe = R"(\\.\pipe\aegis_cmd_v1)";
constexpr const char* kEvtPipe = R"(\\.\pipe\aegis_evt_v1)";
constexpr int kReadPollMs = 250;
constexpr int kHeartbeatMs = 1000;
constexpr std::uint32_t kMaxFrameSize = 64 * 1024;

std::uint64_t NowUnixMs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

std::string NewId() {
    static std::atomic<std::uint64_t> seq{1};
    std::ostringstream oss;
    oss << "cpp-" << NowUnixMs() << "-" << seq.fetch_add(1);
    return oss.str();
}

void MpWriteU8(std::vector<std::uint8_t>& out, std::uint8_t v) {
    out.push_back(v);
}

void MpWriteU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(v & 0xff));
}

void MpWriteU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xff));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(v & 0xff));
}

void MpWriteU64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xff));
    }
}

void MpWriteString(std::vector<std::uint8_t>& out, const std::string& s) {
    const auto len = s.size();
    if (len <= 31) {
        out.push_back(static_cast<std::uint8_t>(0xa0 | len));
    } else if (len <= 0xff) {
        out.push_back(0xd9);
        out.push_back(static_cast<std::uint8_t>(len));
    } else if (len <= 0xffff) {
        out.push_back(0xda);
        MpWriteU16(out, static_cast<std::uint16_t>(len));
    } else {
        out.push_back(0xdb);
        MpWriteU32(out, static_cast<std::uint32_t>(len));
    }
    out.insert(out.end(), s.begin(), s.end());
}

void MpWriteBool(std::vector<std::uint8_t>& out, bool v) {
    out.push_back(v ? 0xc3 : 0xc2);
}

void MpWriteNil(std::vector<std::uint8_t>& out) {
    out.push_back(0xc0);
}

void MpWriteUInt(std::vector<std::uint8_t>& out, std::uint64_t v) {
    if (v <= 0x7f) {
        out.push_back(static_cast<std::uint8_t>(v));
    } else if (v <= 0xff) {
        out.push_back(0xcc);
        out.push_back(static_cast<std::uint8_t>(v));
    } else if (v <= 0xffff) {
        out.push_back(0xcd);
        MpWriteU16(out, static_cast<std::uint16_t>(v));
    } else if (v <= 0xffffffffULL) {
        out.push_back(0xce);
        MpWriteU32(out, static_cast<std::uint32_t>(v));
    } else {
        out.push_back(0xcf);
        MpWriteU64(out, v);
    }
}

void MpWriteArrayHeader(std::vector<std::uint8_t>& out, std::size_t count) {
    if (count <= 15) {
        out.push_back(static_cast<std::uint8_t>(0x90 | count));
    } else {
        out.push_back(0xdc);
        MpWriteU16(out, static_cast<std::uint16_t>(count));
    }
}

void MpWriteMapHeader(std::vector<std::uint8_t>& out, std::size_t count) {
    if (count <= 15) {
        out.push_back(static_cast<std::uint8_t>(0x80 | count));
    } else {
        out.push_back(0xde);
        MpWriteU16(out, static_cast<std::uint16_t>(count));
    }
}

struct MpReader {
    const std::vector<std::uint8_t>& buf;
    std::size_t pos = 0;

    bool ReadByte(std::uint8_t& out) {
        if (pos >= buf.size()) return false;
        out = buf[pos++];
        return true;
    }

    bool PeekByte(std::uint8_t& out) const {
        if (pos >= buf.size()) return false;
        out = buf[pos];
        return true;
    }

    bool ReadN(std::size_t n, const std::uint8_t*& out) {
        if (pos + n > buf.size()) return false;
        out = buf.data() + pos;
        pos += n;
        return true;
    }
};

bool MpReadUInt(MpReader& r, std::uint64_t& out);
bool MpSkip(MpReader& r);
bool MpToJson(MpReader& r, std::string& out_json);
std::string JsonEscape(const std::string& input);

bool MpReadString(MpReader& r, std::string& out) {
    out.clear();
    std::uint8_t b = 0;
    if (!r.ReadByte(b)) return false;
    std::size_t len = 0;
    if ((b & 0xe0) == 0xa0) {
        len = b & 0x1f;
    } else if (b == 0xd9) {
        std::uint8_t l = 0;
        if (!r.ReadByte(l)) return false;
        len = l;
    } else if (b == 0xda) {
        const std::uint8_t* p = nullptr;
        if (!r.ReadN(2, p)) return false;
        len = (static_cast<std::size_t>(p[0]) << 8) | p[1];
    } else if (b == 0xdb) {
        const std::uint8_t* p = nullptr;
        if (!r.ReadN(4, p)) return false;
        len = (static_cast<std::size_t>(p[0]) << 24) | (static_cast<std::size_t>(p[1]) << 16) |
              (static_cast<std::size_t>(p[2]) << 8) | p[3];
    } else {
        return false;
    }
    const std::uint8_t* p = nullptr;
    if (!r.ReadN(len, p)) return false;
    out.assign(reinterpret_cast<const char*>(p), len);
    return true;
}

bool MpReadBool(MpReader& r, bool& out) {
    std::uint8_t b = 0;
    if (!r.ReadByte(b)) return false;
    if (b == 0xc2) {
        out = false;
        return true;
    }
    if (b == 0xc3) {
        out = true;
        return true;
    }
    return false;
}

bool MpReadUInt(MpReader& r, std::uint64_t& out) {
    std::uint8_t b = 0;
    if (!r.ReadByte(b)) return false;
    if (b <= 0x7f) {
        out = b;
        return true;
    }
    const std::uint8_t* p = nullptr;
    if (b == 0xcc) {
        if (!r.ReadN(1, p)) return false;
        out = p[0];
        return true;
    }
    if (b == 0xcd) {
        if (!r.ReadN(2, p)) return false;
        out = (static_cast<std::uint64_t>(p[0]) << 8) | p[1];
        return true;
    }
    if (b == 0xce) {
        if (!r.ReadN(4, p)) return false;
        out = (static_cast<std::uint64_t>(p[0]) << 24) | (static_cast<std::uint64_t>(p[1]) << 16) |
              (static_cast<std::uint64_t>(p[2]) << 8) | p[3];
        return true;
    }
    if (b == 0xcf) {
        if (!r.ReadN(8, p)) return false;
        out = 0;
        for (int i = 0; i < 8; ++i) out = (out << 8) | p[i];
        return true;
    }
    return false;
}

bool MpReadMapHeader(MpReader& r, std::size_t& count) {
    std::uint8_t b = 0;
    if (!r.ReadByte(b)) return false;
    if ((b & 0xf0) == 0x80) {
        count = b & 0x0f;
        return true;
    }
    const std::uint8_t* p = nullptr;
    if (b == 0xde) {
        if (!r.ReadN(2, p)) return false;
        count = (static_cast<std::size_t>(p[0]) << 8) | p[1];
        return true;
    }
    if (b == 0xdf) {
        if (!r.ReadN(4, p)) return false;
        count = (static_cast<std::size_t>(p[0]) << 24) | (static_cast<std::size_t>(p[1]) << 16) |
                (static_cast<std::size_t>(p[2]) << 8) | p[3];
        return true;
    }
    return false;
}

bool MpReadArrayHeader(MpReader& r, std::size_t& count) {
    std::uint8_t b = 0;
    if (!r.ReadByte(b)) return false;
    if ((b & 0xf0) == 0x90) {
        count = b & 0x0f;
        return true;
    }
    const std::uint8_t* p = nullptr;
    if (b == 0xdc) {
        if (!r.ReadN(2, p)) return false;
        count = (static_cast<std::size_t>(p[0]) << 8) | p[1];
        return true;
    }
    if (b == 0xdd) {
        if (!r.ReadN(4, p)) return false;
        count = (static_cast<std::size_t>(p[0]) << 24) | (static_cast<std::size_t>(p[1]) << 16) |
                (static_cast<std::size_t>(p[2]) << 8) | p[3];
        return true;
    }
    return false;
}

bool MpToJsonMap(MpReader& r, std::string& out_json) {
    std::size_t n = 0;
    if (!MpReadMapHeader(r, n)) return false;
    std::ostringstream os;
    os << "{";
    for (std::size_t i = 0; i < n; ++i) {
        std::string key;
        if (!MpReadString(r, key)) return false;
        if (i > 0) os << ",";
        os << "\"" << JsonEscape(key) << "\":";
        std::string value_json;
        if (!MpToJson(r, value_json)) return false;
        os << value_json;
    }
    os << "}";
    out_json = os.str();
    return true;
}

bool MpToJsonArray(MpReader& r, std::string& out_json) {
    std::size_t n = 0;
    if (!MpReadArrayHeader(r, n)) return false;
    std::ostringstream os;
    os << "[";
    for (std::size_t i = 0; i < n; ++i) {
        if (i > 0) os << ",";
        std::string item_json;
        if (!MpToJson(r, item_json)) return false;
        os << item_json;
    }
    os << "]";
    out_json = os.str();
    return true;
}

bool MpToJson(MpReader& r, std::string& out_json) {
    std::uint8_t b = 0;
    if (!r.PeekByte(b)) return false;

    if ((b & 0xf0) == 0x80 || b == 0xde || b == 0xdf) {
        return MpToJsonMap(r, out_json);
    }
    if ((b & 0xf0) == 0x90 || b == 0xdc || b == 0xdd) {
        return MpToJsonArray(r, out_json);
    }
    if ((b & 0xe0) == 0xa0 || b == 0xd9 || b == 0xda || b == 0xdb) {
        std::string s;
        if (!MpReadString(r, s)) return false;
        out_json = std::string("\"") + JsonEscape(s) + "\"";
        return true;
    }
    if (b == 0xc0) {
        std::uint8_t tmp = 0;
        if (!r.ReadByte(tmp)) return false;
        out_json = "null";
        return true;
    }
    if (b == 0xc2 || b == 0xc3) {
        bool v = false;
        if (!MpReadBool(r, v)) return false;
        out_json = v ? "true" : "false";
        return true;
    }
    if (b <= 0x7f || b == 0xcc || b == 0xcd || b == 0xce || b == 0xcf) {
        std::uint64_t u = 0;
        if (!MpReadUInt(r, u)) return false;
        out_json = std::to_string(u);
        return true;
    }

    // Unsupported MsgPack type in current protocol subset (e.g., float/signed/ext/bin).
    return false;
}

bool MpSkip(MpReader& r) {
    std::uint8_t b = 0;
    if (!r.PeekByte(b)) return false;

    if ((b & 0x80) == 0x00 || (b & 0xe0) == 0xa0 || (b & 0xf0) == 0x80 || (b & 0xf0) == 0x90) {
        // fixint / fixstr / fixmap / fixarray handled by typed readers after consuming header.
    }

    if ((b & 0xe0) == 0xa0) {
        std::string s;
        return MpReadString(r, s);
    }
    if ((b & 0xf0) == 0x80) {
        std::size_t n = 0;
        if (!MpReadMapHeader(r, n)) return false;
        for (std::size_t i = 0; i < n; ++i) {
            if (!MpSkip(r)) return false;
            if (!MpSkip(r)) return false;
        }
        return true;
    }
    if ((b & 0xf0) == 0x90) {
        std::size_t n = 0;
        if (!MpReadArrayHeader(r, n)) return false;
        for (std::size_t i = 0; i < n; ++i) {
            if (!MpSkip(r)) return false;
        }
        return true;
    }
    if (b <= 0x7f) {
        std::uint64_t u = 0;
        return MpReadUInt(r, u);
    }
    if (b == 0xc0) {
        std::uint8_t tmp = 0;
        return r.ReadByte(tmp);
    }
    if (b == 0xc2 || b == 0xc3) {
        bool v = false;
        return MpReadBool(r, v);
    }
    if (b == 0xcc || b == 0xcd || b == 0xce || b == 0xcf) {
        std::uint64_t u = 0;
        return MpReadUInt(r, u);
    }
    if (b == 0xd9 || b == 0xda || b == 0xdb) {
        std::string s;
        return MpReadString(r, s);
    }
    if (b == 0xde || b == 0xdf) {
        std::size_t n = 0;
        if (!MpReadMapHeader(r, n)) return false;
        for (std::size_t i = 0; i < n; ++i) {
            if (!MpSkip(r)) return false;
            if (!MpSkip(r)) return false;
        }
        return true;
    }
    if (b == 0xdc || b == 0xdd) {
        std::size_t n = 0;
        if (!MpReadArrayHeader(r, n)) return false;
        for (std::size_t i = 0; i < n; ++i) {
            if (!MpSkip(r)) return false;
        }
        return true;
    }
    return false;
}

struct ParsedEnvelopeMeta {
    std::string type;
    std::string request_id;
    std::string scene_name;
    std::string reason;
};

std::string JsonEscape(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (char ch : input) {
        switch (ch) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out.push_back(ch);
            break;
        }
    }
    return out;
}

std::string BuildIncomingEnvelopeJson(const ParsedEnvelopeMeta& meta) {
    std::ostringstream os;
    os << "{";
    os << "\"v\":1,";
    os << "\"id\":\"cpp-incoming-meta\",";
    os << "\"ts_unix_ms\":" << NowUnixMs() << ",";
    os << "\"type\":\"" << JsonEscape(meta.type) << "\",";
    os << "\"payload\":";
    if (meta.type == "switch_scene") {
        os << "{";
        os << "\"request_id\":";
        if (meta.request_id.empty()) {
            os << "null";
        } else {
            os << "\"" << JsonEscape(meta.request_id) << "\"";
        }
        os << ",";
        os << "\"scene_name\":";
        if (meta.scene_name.empty()) {
            os << "null";
        } else {
            os << "\"" << JsonEscape(meta.scene_name) << "\"";
        }
        os << ",";
        os << "\"reason\":";
        if (meta.reason.empty()) {
            os << "null";
        } else {
            os << "\"" << JsonEscape(meta.reason) << "\"";
        }
        os << "}";
    } else {
        os << "{}";
    }
    os << "}";
    return os.str();
}

bool TryBuildIncomingEnvelopeJsonFromFrame(
    const std::vector<std::uint8_t>& frame,
    std::string& out_json) {
    MpReader r{frame};
    std::string json;
    if (!MpToJson(r, json)) {
        return false;
    }
    if (r.pos != frame.size()) {
        return false;
    }
    out_json = std::move(json);
    return true;
}

bool MpEnvelopeTypeAndSwitchSceneMeta(
    const std::vector<std::uint8_t>& frame,
    ParsedEnvelopeMeta& out_meta) {
    out_meta = {};
    MpReader r{frame};
    std::size_t n = 0;
    if (!MpReadMapHeader(r, n)) return false;

    std::vector<std::uint8_t> payload_bytes;
    for (std::size_t i = 0; i < n; ++i) {
        std::string key;
        if (!MpReadString(r, key)) return false;
        if (key == "type") {
            if (!MpReadString(r, out_meta.type)) return false;
        } else if (key == "payload") {
            // Capture payload by reading exact object range.
            const std::size_t start = r.pos;
            if (!MpSkip(r)) return false;
            payload_bytes.assign(frame.begin() + static_cast<std::ptrdiff_t>(start),
                                 frame.begin() + static_cast<std::ptrdiff_t>(r.pos));
        } else {
            if (!MpSkip(r)) return false;
        }
    }

    if (out_meta.type != "switch_scene" || payload_bytes.empty()) {
        return !out_meta.type.empty();
    }

    MpReader pr{payload_bytes};
    std::size_t pn = 0;
    if (!MpReadMapHeader(pr, pn)) return true;
    for (std::size_t i = 0; i < pn; ++i) {
        std::string key;
        if (!MpReadString(pr, key)) return true;
        if (key == "request_id") {
            std::string rid;
            if (MpReadString(pr, rid)) out_meta.request_id = rid;
            else return true;
        } else if (key == "scene_name") {
            std::string scene_name;
            if (MpReadString(pr, scene_name)) out_meta.scene_name = scene_name;
            else return true;
        } else if (key == "reason") {
            std::string reason;
            if (MpReadString(pr, reason)) out_meta.reason = reason;
            else return true;
        } else {
            if (!MpSkip(pr)) return true;
        }
    }
    return true;
}

std::vector<std::uint8_t> BuildEnvelopeHello() {
    std::vector<std::uint8_t> out;
    // envelope fields: v,id,ts_unix_ms,type,priority,payload
    MpWriteMapHeader(out, 6);
    MpWriteString(out, "v");
    MpWriteUInt(out, 1);
    MpWriteString(out, "id");
    MpWriteString(out, NewId());
    MpWriteString(out, "ts_unix_ms");
    MpWriteUInt(out, NowUnixMs());
    MpWriteString(out, "type");
    MpWriteString(out, "hello");
    MpWriteString(out, "priority");
    MpWriteString(out, "high");
    MpWriteString(out, "payload");
    MpWriteMapHeader(out, 4);
    MpWriteString(out, "plugin_version");
    MpWriteString(out, "0.0.3-cpp-shim");
    MpWriteString(out, "protocol_version");
    MpWriteUInt(out, 1);
    MpWriteString(out, "obs_pid");
    MpWriteUInt(out, 0);
    MpWriteString(out, "capabilities");
    MpWriteArrayHeader(out, 3);
    MpWriteString(out, "scene_switch");
    MpWriteString(out, "dock");
    MpWriteString(out, "restart_hint");
    return out;
}

std::vector<std::uint8_t> BuildEnvelopeRequestStatus() {
    std::vector<std::uint8_t> out;
    MpWriteMapHeader(out, 6);
    MpWriteString(out, "v");
    MpWriteUInt(out, 1);
    MpWriteString(out, "id");
    MpWriteString(out, NewId());
    MpWriteString(out, "ts_unix_ms");
    MpWriteUInt(out, NowUnixMs());
    MpWriteString(out, "type");
    MpWriteString(out, "request_status");
    MpWriteString(out, "priority");
    MpWriteString(out, "high");
    MpWriteString(out, "payload");
    MpWriteMapHeader(out, 0);
    return out;
}

std::vector<std::uint8_t> BuildEnvelopePing() {
    std::vector<std::uint8_t> out;
    MpWriteMapHeader(out, 6);
    MpWriteString(out, "v");
    MpWriteUInt(out, 1);
    MpWriteString(out, "id");
    MpWriteString(out, NewId());
    MpWriteString(out, "ts_unix_ms");
    MpWriteUInt(out, NowUnixMs());
    MpWriteString(out, "type");
    MpWriteString(out, "ping");
    MpWriteString(out, "priority");
    MpWriteString(out, "normal");
    MpWriteString(out, "payload");
    MpWriteMapHeader(out, 1);
    MpWriteString(out, "nonce");
    MpWriteString(out, NewId());
    return out;
}

std::vector<std::uint8_t> BuildEnvelopeSetModeRequest(const std::string& mode) {
    std::vector<std::uint8_t> out;
    MpWriteMapHeader(out, 6);
    MpWriteString(out, "v");
    MpWriteUInt(out, 1);
    MpWriteString(out, "id");
    MpWriteString(out, NewId());
    MpWriteString(out, "ts_unix_ms");
    MpWriteUInt(out, NowUnixMs());
    MpWriteString(out, "type");
    MpWriteString(out, "set_mode_request");
    MpWriteString(out, "priority");
    MpWriteString(out, "high");
    MpWriteString(out, "payload");
    MpWriteMapHeader(out, 1);
    MpWriteString(out, "mode");
    MpWriteString(out, mode);
    return out;
}

std::vector<std::uint8_t> BuildEnvelopeSetSettingRequest(const std::string& key, bool value) {
    std::vector<std::uint8_t> out;
    MpWriteMapHeader(out, 6);
    MpWriteString(out, "v");
    MpWriteUInt(out, 1);
    MpWriteString(out, "id");
    MpWriteString(out, NewId());
    MpWriteString(out, "ts_unix_ms");
    MpWriteUInt(out, NowUnixMs());
    MpWriteString(out, "type");
    MpWriteString(out, "set_setting_request");
    MpWriteString(out, "priority");
    MpWriteString(out, "high");
    MpWriteString(out, "payload");
    MpWriteMapHeader(out, 2);
    MpWriteString(out, "key");
    MpWriteString(out, key);
    MpWriteString(out, "value");
    MpWriteBool(out, value);
    return out;
}

std::vector<std::uint8_t> BuildEnvelopeSceneSwitchResult(
    const std::string& request_id,
    bool ok,
    const std::string& error) {
    std::vector<std::uint8_t> out;
    MpWriteMapHeader(out, 6);
    MpWriteString(out, "v");
    MpWriteUInt(out, 1);
    MpWriteString(out, "id");
    MpWriteString(out, NewId());
    MpWriteString(out, "ts_unix_ms");
    MpWriteUInt(out, NowUnixMs());
    MpWriteString(out, "type");
    MpWriteString(out, "scene_switch_result");
    MpWriteString(out, "priority");
    MpWriteString(out, "high");
    MpWriteString(out, "payload");
    MpWriteMapHeader(out, 3);
    MpWriteString(out, "request_id");
    MpWriteString(out, request_id);
    MpWriteString(out, "ok");
    MpWriteBool(out, ok);
    MpWriteString(out, "error");
    if (ok || error.empty()) {
        MpWriteNil(out);
    } else {
        MpWriteString(out, error);
    }
    return out;
}

std::vector<std::uint8_t> BuildEnvelopeSceneSwitchResultOk(const std::string& request_id) {
    return BuildEnvelopeSceneSwitchResult(request_id, true, "");
}

std::vector<std::uint8_t> BuildEnvelopeObsShutdownNotice(const std::string& reason) {
    std::vector<std::uint8_t> out;
    MpWriteMapHeader(out, 6);
    MpWriteString(out, "v");
    MpWriteUInt(out, 1);
    MpWriteString(out, "id");
    MpWriteString(out, NewId());
    MpWriteString(out, "ts_unix_ms");
    MpWriteUInt(out, NowUnixMs());
    MpWriteString(out, "type");
    MpWriteString(out, "obs_shutdown_notice");
    MpWriteString(out, "priority");
    MpWriteString(out, "high");
    MpWriteString(out, "payload");
    MpWriteMapHeader(out, 1);
    MpWriteString(out, "reason");
    MpWriteString(out, reason.empty() ? "obs_module_unload" : reason);
    return out;
}
}

namespace aegis {

IpcClient::IpcClient() = default;

IpcClient::~IpcClient() {
    Stop();
}

void IpcClient::SetLogger(LogFn logger) {
    logger_ = std::move(logger);
}

void IpcClient::SetCallbacks(Callbacks callbacks) {
    callbacks_ = std::move(callbacks);
}

void IpcClient::SetAutoAckSwitchScene(bool enabled) {
    auto_ack_switch_scene_ = enabled;
}

void IpcClient::QueueRequestStatus() {
    const bool was_pending = pending_request_status_.exchange(true);
    if (!was_pending) {
        Log("queued request_status");
    }
}

void IpcClient::QueueSceneSwitchResult(
    const std::string& request_id,
    bool ok,
    const std::string& error) {
    if (request_id.empty()) {
        Log("QueueSceneSwitchResult ignored empty request_id");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(pending_scene_results_mu_);
        pending_scene_results_.emplace_back(request_id, ok, error);
    }
    std::ostringstream oss;
    oss << "queued scene_switch_result request_id=" << request_id << " ok=" << (ok ? "true" : "false");
    if (!ok && !error.empty()) {
        oss << " error=" << error;
    }
    Log(oss.str());
}

void IpcClient::QueueSetModeRequest(const std::string& mode) {
    if (mode.empty()) {
        Log("QueueSetModeRequest ignored empty mode");
        return;
    }
    bool replaced_pending = false;
    {
        std::lock_guard<std::mutex> lock(pending_set_mode_mu_);
        if (!pending_set_modes_.empty()) {
            replaced_pending = true;
            pending_set_modes_.clear();
        }
        pending_set_modes_.push_back(mode);
    }
    std::ostringstream oss;
    oss << "queued set_mode_request mode=" << mode;
    if (replaced_pending) {
        oss << " detail=coalesced_latest";
    }
    Log(oss.str());
}

void IpcClient::QueueSetSettingRequest(const std::string& key, bool value) {
    if (key.empty()) {
        Log("QueueSetSettingRequest ignored empty key");
        return;
    }
    bool replaced_pending = false;
    {
        std::lock_guard<std::mutex> lock(pending_set_setting_mu_);
        for (auto& item : pending_set_settings_) {
            if (item.first == key) {
                item.second = value;
                replaced_pending = true;
                break;
            }
        }
        if (!replaced_pending) {
            pending_set_settings_.emplace_back(key, value);
        }
    }
    std::ostringstream oss;
    oss << "queued set_setting_request key=" << key << " value=" << (value ? "true" : "false");
    if (replaced_pending) {
        oss << " detail=coalesced_by_key";
    }
    Log(oss.str());
}

void IpcClient::QueueObsShutdownNotice(const std::string& reason) {
    {
        std::lock_guard<std::mutex> lock(pending_shutdown_notices_mu_);
        pending_shutdown_notices_.push_back(reason.empty() ? "obs_module_unload" : reason);
    }
    std::ostringstream oss;
    oss << "queued obs_shutdown_notice reason=" << (reason.empty() ? "obs_module_unload" : reason);
    Log(oss.str());
}

bool IpcClient::IsRunning() const {
    return running_.load();
}

void IpcClient::Start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }

    worker_ = std::thread([this] { WorkerLoop(); });
}

void IpcClient::Stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }

    DisconnectPipes();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void IpcClient::WorkerLoop() {
    Log("ipc worker started");
    while (running_.load()) {
        if (!ConnectPipes()) {
            SleepInterruptible(250);
            continue;
        }

        ConnectedSessionLoop();
        DisconnectPipes();
    }
    Log("ipc worker stopped");
}

void IpcClient::ConnectedSessionLoop() {
    handshake_sent_ = false;
    request_status_sent_ = false;

    Log("ipc connected (session loop)");

    auto last_ping = std::chrono::steady_clock::now();
    bool logged_codec_gate = false;

    while (running_.load()) {
        // Protocol hook points (currently gated until MsgPack codec implementation lands).
        if (!handshake_sent_) {
            if (!SendHello()) {
                if (!logged_codec_gate) {
                    Log("hello send failed; ending session for reconnect");
                    logged_codec_gate = true;
                }
                break;
            } else {
                handshake_sent_ = true;
            }
        } else if (!request_status_sent_) {
            if (!SendRequestStatus()) {
                Log("request_status send failed; ending session for reconnect");
                break;
            }
            request_status_sent_ = true;
            // Initial session snapshot satisfies any queued refresh that arrived before
            // the first request_status was sent (e.g., dock page ready during handshake).
            pending_request_status_.store(false);
        }

        DrainPendingSetModeRequests();
        DrainPendingSetSettingRequests();
        DrainPendingSceneSwitchResults();
        DrainPendingShutdownNotices();

        if (handshake_sent_ && request_status_sent_ && pending_request_status_.exchange(false)) {
            if (!SendRequestStatus()) {
                Log("queued request_status send failed; ending session for reconnect");
                break;
            }
            Log("sent queued request_status");
        }

        auto now = std::chrono::steady_clock::now();
        if (handshake_sent_ &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_ping).count() >=
                kHeartbeatMs) {
            if (!SendPing()) {
                Log("ping send failed; ending session for reconnect");
                break;
            }
            last_ping = now;
        }

        std::vector<std::uint8_t> frame;
        const ReadFrameResult read_result = TryReadFrame(frame, kReadPollMs);
        if (read_result == ReadFrameResult::Frame) {
            if (!HandleIncomingFrame(frame)) {
                Log("ipc session ending after frame handling failure");
                break;
            }
            continue;
        }

        if (read_result == ReadFrameResult::Disconnected) {
            Log("read failed/disconnected; ending session for reconnect");
            break;
        }

        // timeout/no data path; continue polling
    }
}

bool IpcClient::ConnectPipes() {
#ifdef _WIN32
    DisconnectPipes();

    HANDLE cmd = CreateFileA(
        kCmdPipe,
        GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (cmd == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        std::ostringstream oss;
        oss << "cmd pipe connect retry (err=" << err << ")";
        Log(oss.str());
        return false;
    }

    HANDLE evt = CreateFileA(
        kEvtPipe,
        GENERIC_READ,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (evt == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        CloseHandle(cmd);
        std::ostringstream oss;
        oss << "evt pipe connect retry (err=" << err << ")";
        Log(oss.str());
        return false;
    }

    cmd_pipe_ = cmd;
    evt_pipe_ = evt;
    Log("named pipes opened");
    if (callbacks_.on_pipe_state) {
        callbacks_.on_pipe_state(true);
    }
    return true;
#else
    return false;
#endif
}

IpcClient::PipeReadReadyResult IpcClient::WaitForPipeReadable(void* pipe_handle, int timeout_ms) {
#ifdef _WIN32
    if (!pipe_handle) {
        return PipeReadReadyResult::Disconnected;
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (running_.load()) {
        DWORD bytes_available = 0;
        if (!PeekNamedPipe(
                static_cast<HANDLE>(pipe_handle), nullptr, 0, nullptr, &bytes_available, nullptr)) {
            return PipeReadReadyResult::Disconnected;
        }
        if (bytes_available > 0) {
            return PipeReadReadyResult::Ready;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return PipeReadReadyResult::Timeout;
        }
        SleepInterruptible(20);
    }
    return PipeReadReadyResult::Disconnected;
#else
    (void)pipe_handle;
    (void)timeout_ms;
    return PipeReadReadyResult::Disconnected;
#endif
}

bool IpcClient::ReadExact(void* pipe_handle, void* dst, std::uint32_t len) {
#ifdef _WIN32
    auto* out = static_cast<std::uint8_t*>(dst);
    std::uint32_t total = 0;
    while (running_.load() && total < len) {
        DWORD got = 0;
        BOOL ok = ReadFile(
            static_cast<HANDLE>(pipe_handle), out + total, len - total, &got, nullptr);
        if (!ok || got == 0) {
            return false;
        }
        total += got;
    }
    return total == len;
#else
    (void)pipe_handle;
    (void)dst;
    (void)len;
    return false;
#endif
}

bool IpcClient::WriteAll(void* pipe_handle, const void* src, std::uint32_t len) {
#ifdef _WIN32
    auto* in = static_cast<const std::uint8_t*>(src);
    std::uint32_t total = 0;
    while (running_.load() && total < len) {
        DWORD wrote = 0;
        BOOL ok = WriteFile(
            static_cast<HANDLE>(pipe_handle), in + total, len - total, &wrote, nullptr);
        if (!ok || wrote == 0) {
            return false;
        }
        total += wrote;
    }
    return total == len;
#else
    (void)pipe_handle;
    (void)src;
    (void)len;
    return false;
#endif
}

IpcClient::ReadFrameResult IpcClient::TryReadFrame(
    std::vector<std::uint8_t>& out_payload,
    int timeout_ms) {
    out_payload.clear();
#ifdef _WIN32
    const PipeReadReadyResult ready = WaitForPipeReadable(evt_pipe_, timeout_ms);
    if (ready == PipeReadReadyResult::Timeout) {
        return ReadFrameResult::Timeout;
    }
    if (ready == PipeReadReadyResult::Disconnected) {
        return ReadFrameResult::Disconnected;
    }

    std::uint32_t len_le = 0;
    if (!ReadExact(evt_pipe_, &len_le, sizeof(len_le))) {
        Log("evt pipe read failed (frame length)");
        return ReadFrameResult::Disconnected;
    }
    if (len_le == 0 || len_le > kMaxFrameSize) {
        Log("evt pipe invalid frame length");
        return ReadFrameResult::Disconnected;
    }

    out_payload.resize(len_le);
    if (!ReadExact(evt_pipe_, out_payload.data(), len_le)) {
        Log("evt pipe read failed (frame payload)");
        out_payload.clear();
        return ReadFrameResult::Disconnected;
    }
    return ReadFrameResult::Frame;
#else
    (void)timeout_ms;
    return ReadFrameResult::Disconnected;
#endif
}

bool IpcClient::WriteFrame(const std::vector<std::uint8_t>& payload) {
    if (payload.empty() || payload.size() > kMaxFrameSize) {
        return false;
    }
#ifdef _WIN32
    std::uint32_t len = static_cast<std::uint32_t>(payload.size());
    if (!WriteAll(cmd_pipe_, &len, sizeof(len))) {
        return false;
    }
    if (!WriteAll(cmd_pipe_, payload.data(), len)) {
        return false;
    }
    FlushFileBuffers(static_cast<HANDLE>(cmd_pipe_));
    return true;
#else
    return false;
#endif
}

bool IpcClient::SendHello() {
    return WriteFrame(BuildEnvelopeHello());
}

bool IpcClient::SendRequestStatus() {
    return WriteFrame(BuildEnvelopeRequestStatus());
}

bool IpcClient::SendPing() {
    return WriteFrame(BuildEnvelopePing());
}

bool IpcClient::SendSetModeRequest(const std::string& mode) {
    return WriteFrame(BuildEnvelopeSetModeRequest(mode));
}

bool IpcClient::SendSetSettingRequest(const std::string& key, bool value) {
    return WriteFrame(BuildEnvelopeSetSettingRequest(key, value));
}

bool IpcClient::SendSceneSwitchResult(
    const std::string& request_id,
    bool ok,
    const std::string& error) {
    return WriteFrame(BuildEnvelopeSceneSwitchResult(request_id, ok, error));
}

bool IpcClient::SendObsShutdownNotice(const std::string& reason) {
    return WriteFrame(BuildEnvelopeObsShutdownNotice(reason));
}

bool IpcClient::SendSceneSwitchResultOk(const std::string& request_id) {
    return WriteFrame(BuildEnvelopeSceneSwitchResultOk(request_id));
}

void IpcClient::DrainPendingSetModeRequests() {
    if (!handshake_sent_) {
        return;
    }
    std::vector<std::string> pending;
    {
        std::lock_guard<std::mutex> lock(pending_set_mode_mu_);
        if (pending_set_modes_.empty()) {
            return;
        }
        pending.swap(pending_set_modes_);
    }
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const auto& mode = pending[i];
        if (!SendSetModeRequest(mode)) {
            std::ostringstream oss;
            oss << "failed to send queued set_mode_request mode=" << mode;
            Log(oss.str());
            std::lock_guard<std::mutex> lock(pending_set_mode_mu_);
            pending_set_modes_.insert(
                pending_set_modes_.begin(),
                std::make_move_iterator(pending.begin() + static_cast<std::ptrdiff_t>(i)),
                std::make_move_iterator(pending.end()));
            break;
        }
        std::ostringstream oss;
        oss << "sent queued set_mode_request mode=" << mode;
        Log(oss.str());
    }
}

void IpcClient::DrainPendingSetSettingRequests() {
    if (!handshake_sent_) {
        return;
    }
    std::vector<std::pair<std::string, bool>> pending;
    {
        std::lock_guard<std::mutex> lock(pending_set_setting_mu_);
        if (pending_set_settings_.empty()) {
            return;
        }
        pending.swap(pending_set_settings_);
    }
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const auto& item = pending[i];
        if (!SendSetSettingRequest(item.first, item.second)) {
            std::ostringstream oss;
            oss << "failed to send queued set_setting_request key=" << item.first;
            Log(oss.str());
            std::lock_guard<std::mutex> lock(pending_set_setting_mu_);
            pending_set_settings_.insert(
                pending_set_settings_.begin(),
                std::make_move_iterator(pending.begin() + static_cast<std::ptrdiff_t>(i)),
                std::make_move_iterator(pending.end()));
            break;
        }
        std::ostringstream oss;
        oss << "sent queued set_setting_request key=" << item.first
            << " value=" << (item.second ? "true" : "false");
        Log(oss.str());
    }
}

void IpcClient::DrainPendingSceneSwitchResults() {
    if (!handshake_sent_) {
        return;
    }
    std::vector<std::tuple<std::string, bool, std::string>> pending;
    {
        std::lock_guard<std::mutex> lock(pending_scene_results_mu_);
        if (pending_scene_results_.empty()) {
            return;
        }
        pending.swap(pending_scene_results_);
    }

    for (std::size_t i = 0; i < pending.size(); ++i) {
        const auto& item = pending[i];
        const auto& request_id = std::get<0>(item);
        const bool ok = std::get<1>(item);
        const auto& error = std::get<2>(item);
        if (!SendSceneSwitchResult(request_id, ok, error)) {
            std::ostringstream oss;
            oss << "failed to send queued scene_switch_result request_id=" << request_id;
            Log(oss.str());
            // Re-queue unsent current and remaining items for retry on next loop iteration.
            std::lock_guard<std::mutex> lock(pending_scene_results_mu_);
            pending_scene_results_.insert(
                pending_scene_results_.begin(),
                std::make_move_iterator(pending.begin() + static_cast<std::ptrdiff_t>(i)),
                std::make_move_iterator(pending.end()));
            break;
        }
        std::ostringstream oss;
        oss << "sent queued scene_switch_result request_id=" << request_id
            << " ok=" << (ok ? "true" : "false");
        Log(oss.str());
    }
}

void IpcClient::DrainPendingShutdownNotices() {
    if (!handshake_sent_) {
        return;
    }
    std::vector<std::string> pending;
    {
        std::lock_guard<std::mutex> lock(pending_shutdown_notices_mu_);
        if (pending_shutdown_notices_.empty()) {
            return;
        }
        pending.swap(pending_shutdown_notices_);
    }

    for (std::size_t i = 0; i < pending.size(); ++i) {
        const auto& reason = pending[i];
        if (!SendObsShutdownNotice(reason)) {
            std::ostringstream oss;
            oss << "failed to send queued obs_shutdown_notice reason=" << reason;
            Log(oss.str());
            std::lock_guard<std::mutex> lock(pending_shutdown_notices_mu_);
            pending_shutdown_notices_.insert(
                pending_shutdown_notices_.begin(),
                std::make_move_iterator(pending.begin() + static_cast<std::ptrdiff_t>(i)),
                std::make_move_iterator(pending.end()));
            break;
        }
        std::ostringstream oss;
        oss << "sent queued obs_shutdown_notice reason=" << reason;
        Log(oss.str());
    }
}

bool IpcClient::HandleIncomingFrame(const std::vector<std::uint8_t>& payload) {
    ParsedEnvelopeMeta meta;
    if (!MpEnvelopeTypeAndSwitchSceneMeta(payload, meta)) {
        Log("received frame (decode failed)");
        return true;
    }

    std::ostringstream oss;
    oss << "received frame type=" << meta.type;
    Log(oss.str());
    if (callbacks_.on_message_type) {
        callbacks_.on_message_type(meta.type);
    }
    if (callbacks_.on_incoming_envelope_json && !meta.type.empty()) {
        std::string envelope_json;
        if (TryBuildIncomingEnvelopeJsonFromFrame(payload, envelope_json)) {
            callbacks_.on_incoming_envelope_json(envelope_json);
        } else {
            callbacks_.on_incoming_envelope_json(BuildIncomingEnvelopeJson(meta));
        }
    }

    if (meta.type == "switch_scene") {
        if (callbacks_.on_switch_scene_request) {
            callbacks_.on_switch_scene_request(meta.request_id, meta.scene_name, meta.reason);
        }
        if (!meta.request_id.empty()) {
            std::ostringstream ack;
            ack << "switch_scene request_id=" << meta.request_id;
            if (auto_ack_switch_scene_) {
                if (meta.scene_name.empty()) {
                    ack << " auto-ack=error(missing_scene_name)";
                    Log(ack.str());
                    if (!SendSceneSwitchResult(meta.request_id, false, "missing_scene_name")) {
                        Log("failed to send scene_switch_result");
                        return false;
                    }
                } else {
                    ack << " auto-ack=ok";
                    Log(ack.str());
                    if (!SendSceneSwitchResultOk(meta.request_id)) {
                        Log("failed to send scene_switch_result");
                        return false;
                    }
                }
            } else {
                ack << " callback-mode (auto-ack disabled)";
                Log(ack.str());
            }
        } else {
            Log("switch_scene received but request_id missing");
        }
    }
    return true;
}

bool IpcClient::TryExtractStringField(
    const std::vector<std::uint8_t>& payload,
    const char* field_name,
    std::string& out_value) {
    // Temporary best-effort scan: this is NOT a full MessagePack parser.
    // It only helps debugging logs until the real MsgPack codec is implemented.
    out_value.clear();
    if (!field_name || !*field_name) {
        return false;
    }
    const std::string needle(field_name);
    auto it = std::search(payload.begin(), payload.end(), needle.begin(), needle.end());
    if (it == payload.end()) {
        return false;
    }
    auto idx = static_cast<std::size_t>(std::distance(payload.begin(), it) + needle.size());
    // Heuristic: scan forward for printable ASCII sequence after field key.
    while (idx < payload.size() && (payload[idx] < 0x20 || payload[idx] > 0x7e)) {
        ++idx;
    }
    while (idx < payload.size()) {
        unsigned char c = payload[idx];
        if (c < 0x20 || c > 0x7e) {
            break;
        }
        out_value.push_back(static_cast<char>(c));
        ++idx;
    }
    return !out_value.empty();
}

void IpcClient::DisconnectPipes() {
#ifdef _WIN32
    const bool had_any = (cmd_pipe_ != nullptr) || (evt_pipe_ != nullptr);
    if (cmd_pipe_) {
        CloseHandle(static_cast<HANDLE>(cmd_pipe_));
        cmd_pipe_ = nullptr;
    }
    if (evt_pipe_) {
        CloseHandle(static_cast<HANDLE>(evt_pipe_));
        evt_pipe_ = nullptr;
    }
    if (had_any && callbacks_.on_pipe_state) {
        callbacks_.on_pipe_state(false);
    }
#else
    cmd_pipe_ = nullptr;
    evt_pipe_ = nullptr;
#endif
}

void IpcClient::SleepInterruptible(int ms) {
    constexpr int kSliceMs = 50;
    int remaining = ms;
    while (running_.load() && remaining > 0) {
        int step = remaining < kSliceMs ? remaining : kSliceMs;
        std::this_thread::sleep_for(std::chrono::milliseconds(step));
        remaining -= step;
    }
}

void IpcClient::Log(const std::string& msg) const {
    if (logger_) {
        logger_(msg);
    }
}

} // namespace aegis
