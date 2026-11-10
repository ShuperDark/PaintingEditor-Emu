// Multiplayer: the games of a multiplayer game that have this mod tell each other about
// edited paintings and send each other the pictures, as RakNet messages of their own on the
// game's connection (first byte kMsgId; a game without the mod drops them unread).
//
// The host (the game whose world it is) has the say: its edits are the ones saved in the
// world. A joined game (client) shows the host's edits, and sends its own edits to the host,
// which applies them and passes them on to every joined game with the mod.
//
//   client -> host  HELLO                    on connecting (the host answers HELLO and sends
//                                            an EDIT for every edited painting it has loaded)
//   host -> client  EDIT    id, edit         a painting got this picture / size (also when a
//                                            painting with an edit is loaded on the host)
//   client -> host  REQUEST id, edit         the client edited a painting
//   any             GET_FILE name            send me this picture
//   any             FILE    name, size, offset, bytes   (in pieces of kChunk bytes)
//   any             MISSING name             I do not have it
//
// Paintings are named by their entity unique id (the client's copy of a painting has the
// same id as the host's). Pictures are named by their contents (assets.cpp), so a game that
// lacks one asks for it: a client asks the host, the host asks the client that sent the edit.
// Everything runs on the game's main thread (RakPeer::Receive is called by
// RakNetInstance::runEvents, setEdit by the editor and the level).
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "pe.h"

namespace {

constexpr uint8_t kMsgId = 0xE5;  // RakNet user message id (>= ID_USER_PACKET_ENUM, unknown to the game)
constexpr uint8_t kVersion = 1;
constexpr char kChannel = 7;      // ordering channel of our own (the game uses 0)
enum : uint8_t { kHello = 1, kEdit, kRequest, kGetFile, kFile, kMissing };
// RakNet message ids of the connection events
enum : uint8_t { kConnectionAccepted = 0x10, kDisconnected = 0x15, kConnectionLost = 0x16 };

constexpr size_t kChunk = 32 * 1024;
constexpr size_t kMaxFile = 40u << 20;          // the picker gives at most 32 MB
constexpr double kRate = 4.0 * 1024 * 1024;     // bytes of pictures sent per second
constexpr double kFetchTimeout = 30, kRetryAfter = 15;

struct Remote {
    void* peer = nullptr;  // RakPeer
    uint64_t guid = 0;
    g::AddressOrGUID addr{};
};

std::recursive_mutex g_mx;
bool g_isClient = false;  // connected to someone's game
bool g_serverHasMod = false;
Remote g_server;
std::vector<Remote> g_peers;                       // host: joined games with the mod
std::unordered_map<int64_t, Edit> g_serverEdits;   // client: the host's edits (by painting id)
std::unordered_map<int64_t, Edit> g_pending;       // host: clients' edits waiting for their painting's tick
std::map<std::string, uint64_t> g_fileSource;      // host: the client that sent an edit with this picture

struct Fetch {  // a picture being received
    uint64_t from = 0;
    uint32_t total = 0;
    std::vector<uint8_t> data;
    std::vector<uint64_t> waiting;  // host: clients that asked for it meanwhile
    double last = 0;
};
std::map<std::string, Fetch> g_fetch;
struct Wanted { uint64_t from; double nextTry; };
std::map<std::string, Wanted> g_wanted;  // pictures to ask for (retried until they come)

struct Out {  // a picture being sent
    uint64_t to;
    std::string file;
    std::shared_ptr<std::vector<uint8_t>> data;
    size_t offset = 0;
};
std::deque<Out> g_out;
double g_allowance = 0, g_lastPump = 0;

// ---------------------------------------------------------------- messages
struct Writer {
    std::vector<uint8_t> b;
    explicit Writer(uint8_t type) { b = {kMsgId, type}; }
    void u8(uint8_t v) { b.push_back(v); }
    void u32(uint32_t v) { for (int i = 0; i < 4; i++) b.push_back((uint8_t)(v >> (8 * i))); }
    void i64(int64_t v) { for (int i = 0; i < 8; i++) b.push_back((uint8_t)((uint64_t)v >> (8 * i))); }
    void str(const std::string& s) { u8((uint8_t)std::min<size_t>(s.size(), 255)); b.insert(b.end(), s.begin(), s.begin() + std::min<size_t>(s.size(), 255)); }
    void bytes(const uint8_t* p, size_t n) { b.insert(b.end(), p, p + n); }
};
struct Reader {
    const uint8_t* p;
    size_t n, i = 2;  // after the id and the type
    bool ok = true;
    bool need(size_t k) { return ok = ok && i + k <= n; }
    uint8_t u8() { return need(1) ? p[i++] : 0; }
    uint32_t u32() { uint32_t v = 0; if (need(4)) for (int k = 0; k < 4; k++) v |= (uint32_t)p[i++] << (8 * k); return v; }
    int64_t i64() { uint64_t v = 0; if (need(8)) for (int k = 0; k < 8; k++) v |= (uint64_t)p[i++] << (8 * k); return (int64_t)v; }
    std::string str() { size_t k = u8(); if (!need(k)) return ""; std::string s((const char*)p + i, k); i += k; return s; }
};

void send(const Remote& r, const Writer& w) {
    if (r.peer) g::rakSend(r.peer, w.b.data(), (int)w.b.size(), g::kLowPriority, g::kReliableOrdered, kChannel, r.addr);
}

void writeEdit(Writer& w, int64_t id, const Edit& e) {
    w.i64(id);
    w.u8((uint8_t)e.w);
    w.u8((uint8_t)e.h);
    w.u8((uint8_t)(e.rot & 3));
    w.u8(e.flip ? 1 : 0);
    w.str(e.file);
}
bool readEdit(Reader& r, int64_t& id, Edit& e) {
    id = r.i64();
    e.w = r.u8();
    e.h = r.u8();
    e.rot = r.u8();
    int flip = r.u8();
    e.flip = flip != 0;
    e.file = r.str();
    return r.ok && sizeIndexOf(e.w, e.h) >= 0 && e.rot >= 0 && e.rot <= 3 && flip <= 1 && validPictureName(e.file);
}

Remote remoteOf(void* peer, const char* packet) {
    Remote r;
    r.peer = peer;
    memcpy(&r.guid, packet + g::kPacketGuid, 8);
    memcpy(r.addr.raw, packet + g::kPacketGuid, 16);            // RakNetGUID
    memcpy(r.addr.raw + 16, packet, g::kSystemAddressSize);     // SystemAddress
    return r;
}

const Remote* findRemote(uint64_t guid) {
    if (g_isClient && g_server.guid == guid) return &g_server;
    for (auto& r : g_peers)
        if (r.guid == guid) return &r;
    return nullptr;
}

// the local player's level is a joined game's (not the host's)
bool inClientLevel() {
    void* player = g::localPlayer(currentClient());
    void* level = player ? g::entityLevel(player) : nullptr;
    return level && g::levelIsClientSide(level);
}
bool inHostLevel() {
    void* player = g::localPlayer(currentClient());
    void* level = player ? g::entityLevel(player) : nullptr;
    return level && !g::levelIsClientSide(level);
}

void resetClient(const char* why) {
    if (g_isClient) peLog("network: left the host's game (%s)", why);
    g_isClient = false;
    g_serverHasMod = false;
    g_server = Remote();
    g_serverEdits.clear();
    g_fetch.clear();
    g_wanted.clear();
    g_out.clear();
}

void forgetPeer(uint64_t guid) {
    g_peers.erase(std::remove_if(g_peers.begin(), g_peers.end(), [&](const Remote& r) { return r.guid == guid; }), g_peers.end());
    g_out.erase(std::remove_if(g_out.begin(), g_out.end(), [&](const Out& o) { return o.to == guid; }), g_out.end());
    for (auto& [name, f] : g_fetch) f.waiting.erase(std::remove(f.waiting.begin(), f.waiting.end(), guid), f.waiting.end());
    for (auto it = g_fetch.begin(); it != g_fetch.end();) it = it->second.from == guid ? g_fetch.erase(it) : std::next(it);
}

// ---------------------------------------------------------------- pictures
void wantFile(const std::string& file, uint64_t from) {
    if (!from || pictureStored(file) || g_wanted.count(file)) return;
    g_wanted[file] = {from, 0};
}

void startFetch(const std::string& file, const Remote& from) {
    Fetch& f = g_fetch[file];
    f.from = from.guid;
    f.total = 0;
    f.data.clear();
    f.last = peNow();
    Writer w(kGetFile);
    w.str(file);
    send(from, w);
    peLog("network: asking for the picture %s", file.c_str());
}

bool readPicture(const std::string& file, std::vector<uint8_t>& out) {
    FILE* f = fopen((picturesDir() + "/" + file).c_str(), "rb");
    if (!f) return false;
    out.clear();
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && out.size() <= kMaxFile) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return !out.empty() && out.size() <= kMaxFile;
}

void sendMissing(const Remote& to, const std::string& file) {
    Writer w(kMissing);
    w.str(file);
    send(to, w);
}

void queueFile(uint64_t to, const std::string& file, std::shared_ptr<std::vector<uint8_t>> data) {
    for (auto& o : g_out)
        if (o.to == to && o.file == file) return;  // already being sent
    g_out.push_back({to, file, std::move(data), 0});
}

void onGetFile(const Remote& from, const std::string& file) {
    if (!validPictureName(file)) return;
    auto data = std::make_shared<std::vector<uint8_t>>();
    if (readPicture(file, *data)) {
        peLog("network: sending the picture %s (%zu bytes)", file.c_str(), data->size());
        queueFile(from.guid, file, data);
        return;
    }
    if (!g_isClient) {  // host: get it from the client that has it, then pass it on
        auto it = g_fetch.find(file);
        if (it == g_fetch.end()) {
            auto src = g_fileSource.find(file);
            const Remote* r = src != g_fileSource.end() ? findRemote(src->second) : nullptr;
            if (r && r->guid != from.guid) {
                startFetch(file, *r);
                it = g_fetch.find(file);
            }
        }
        if (it != g_fetch.end()) {
            if (std::find(it->second.waiting.begin(), it->second.waiting.end(), from.guid) == it->second.waiting.end())
                it->second.waiting.push_back(from.guid);
            return;
        }
    }
    sendMissing(from, file);
}

void fetchFailed(const std::string& file) {
    auto it = g_fetch.find(file);
    if (it == g_fetch.end()) return;
    for (uint64_t g : it->second.waiting)
        if (const Remote* r = findRemote(g)) sendMissing(*r, file);
    g_fetch.erase(it);
    auto w = g_wanted.find(file);
    if (w != g_wanted.end()) w->second.nextTry = peNow() + kRetryAfter;
}

void onFile(const Remote& from, Reader& r) {
    std::string file = r.str();
    uint32_t total = r.u32(), offset = r.u32();
    if (!r.ok) return;
    auto it = g_fetch.find(file);
    if (it == g_fetch.end() || it->second.from != from.guid) return;
    Fetch& f = it->second;
    size_t n = r.n - r.i;
    if (offset == 0 && f.data.empty()) {
        if (total == 0 || total > kMaxFile) {
            peLog("network: picture %s: bad size %u", file.c_str(), total);
            fetchFailed(file);
            return;
        }
        f.total = total;
        f.data.reserve(total);
    }
    if (total != f.total || offset != f.data.size() || f.data.size() + n > f.total) {
        peLog("network: picture %s: unexpected piece (%u of %u)", file.c_str(), offset, total);
        fetchFailed(file);
        return;
    }
    f.data.insert(f.data.end(), r.p + r.i, r.p + r.n);
    f.last = peNow();
    if (f.data.size() < f.total) return;
    // complete: stored under the name made from its contents, which must be the one asked for
    std::string stored = storePicture(f.data);
    if (stored != file) {
        peLog("network: picture %s: received data does not match (%s)", file.c_str(), stored.c_str());
        fetchFailed(file);
        return;
    }
    peLog("network: received the picture %s (%u bytes)", file.c_str(), f.total);
    refreshAsset(file);
    g_wanted.erase(file);
    if (!f.waiting.empty()) {
        auto data = std::make_shared<std::vector<uint8_t>>(std::move(f.data));
        for (uint64_t g : f.waiting) queueFile(g, file, data);
    }
    g_fetch.erase(it);
}

// ---------------------------------------------------------------- edits
void sendEdit(const Remote& to, uint8_t type, int64_t id, const Edit& e) {
    Writer w(type);
    writeEdit(w, id, e);
    send(to, w);
}

void onHello(const Remote& from, Reader& r) {
    int version = r.u8();
    if (g_isClient && from.guid == g_server.guid) {
        g_serverHasMod = true;
        peLog("network: the host has PaintingEditor (version %d)", version);
        return;
    }
    // host: a joined game with the mod
    forgetPeer(from.guid);
    g_peers.push_back(from);
    Writer w(kHello);
    w.u8(kVersion);
    send(from, w);
    auto edits = allEdits();
    for (auto& [p, e] : edits) sendEdit(from, kEdit, g::entityUniqueId(p), e);
    peLog("network: a joined game has PaintingEditor (version %d); sent %zu edited painting(s)", version, edits.size());
}

void onEdit(const Remote& from, Reader& r) {
    int64_t id;
    Edit e;
    if (!g_isClient || from.guid != g_server.guid || !readEdit(r, id, e)) return;
    g_serverEdits[id] = e;  // applied on the painting's next tick (it may not exist here yet)
    wantFile(e.file, from.guid);
}

void onRequest(const Remote& from, Reader& r) {
    int64_t id;
    Edit e;
    if (g_isClient || !readEdit(r, id, e)) return;
    if (!findRemote(from.guid)) g_peers.push_back(from);
    g_fileSource[e.file] = from.guid;
    g_pending[id] = e;
    peLog("network: a joined game edited painting %lld", (long long)id);
}

void handle(const Remote& from, const uint8_t* d, size_t n) {
    Reader r{d, n};
    switch (d[1]) {
    case kHello: onHello(from, r); break;
    case kEdit: onEdit(from, r); break;
    case kRequest: onRequest(from, r); break;
    case kGetFile: onGetFile(from, r.str()); break;
    case kFile: onFile(from, r); break;
    case kMissing: {
        std::string file = r.str();
        auto it = g_fetch.find(file);
        if (it != g_fetch.end() && it->second.from == from.guid) {
            peLog("network: the other game does not have the picture %s", file.c_str());
            fetchFailed(file);
        }
        break;
    }
    default: break;
    }
}

// ---------------------------------------------------------------- per tick
void pump() {
    double now = peNow();
    if (g_isClient && inHostLevel()) resetClient("playing a world of this game");  // left without a notice
    g_allowance = std::min(g_allowance + (now - g_lastPump) * kRate, 8.0 * kChunk);
    g_lastPump = now;
    // pictures to ask for
    for (auto it = g_wanted.begin(); it != g_wanted.end();) {
        const std::string& file = it->first;
        Wanted& w = it->second;
        if (g_fetch.count(file) || w.nextTry > now) {
            ++it;
            continue;
        }
        if (pictureStored(file)) {
            it = g_wanted.erase(it);
            continue;
        }
        const Remote* r = findRemote(w.from);
        if (!r && g_isClient) r = &g_server;  // the host may have it by now
        if (r) startFetch(file, *r);
        w.nextTry = now + kRetryAfter;
        ++it;
    }
    // stalled transfers
    std::vector<std::string> stalled;
    for (auto& [file, f] : g_fetch)
        if (now - f.last > kFetchTimeout) stalled.push_back(file);
    for (auto& file : stalled) {
        peLog("network: picture %s: no data for %.0f s", file.c_str(), kFetchTimeout);
        fetchFailed(file);
    }
    // pictures being sent, kChunk bytes at a time
    while (!g_out.empty() && g_allowance >= kChunk) {
        Out& o = g_out.front();
        const Remote* r = findRemote(o.to);
        if (!r) {
            g_out.pop_front();
            continue;
        }
        size_t n = std::min(kChunk, o.data->size() - o.offset);
        Writer w(kFile);
        w.str(o.file);
        w.u32((uint32_t)o.data->size());
        w.u32((uint32_t)o.offset);
        w.bytes(o.data->data() + o.offset, n);
        send(*r, w);
        g_allowance -= n;
        o.offset += n;
        if (o.offset >= o.data->size()) g_out.pop_front();
    }
}

using ReceiveFn = void* (*)(void*);
ReceiveFn o_receive;

// RakPeer::Receive: takes our messages out, watches the connection events, and lets the
// game have everything else.
void* h_receive(void* peer) {
    for (;;) {
        char* p = (char*)o_receive(peer);
        std::lock_guard lk(g_mx);
        if (!p) {
            pump();
            return nullptr;
        }
        uint32_t len = *(uint32_t*)(p + g::kPacketLength);
        const uint8_t* d = *(const uint8_t**)(p + g::kPacketData);
        if (!d || len < 1) return p;
        if (d[0] == kMsgId) {
            if (len >= 2) handle(remoteOf(peer, p), d, len);
            g::rakDeallocate(peer, p);
            continue;
        }
        if (d[0] == kConnectionAccepted) {  // we joined someone's game
            resetClient("joined another game");
            g_isClient = true;
            g_server = remoteOf(peer, p);
            Writer w(kHello);
            w.u8(kVersion);
            send(g_server, w);
        } else if (d[0] == kDisconnected || d[0] == kConnectionLost) {
            Remote r = remoteOf(peer, p);
            if (g_isClient && r.guid == g_server.guid) resetClient(d[0] == kDisconnected ? "disconnected" : "connection lost");
            else forgetPeer(r.guid);
        }
        return p;
    }
}

}  // namespace

void netEditApplied(void* p, const Edit& e, EditFrom from) {
    std::lock_guard lk(g_mx);
    int64_t id = g::entityUniqueId(p);
    if (g_isClient && !inHostLevel()) {
        g_serverEdits[id] = e;
        if (from == EditFrom::Local) {
            sendEdit(g_server, kRequest, id, e);
            if (!g_serverHasMod) peLog("network: the host does not have PaintingEditor; only this game shows the edit");
        } else if (from == EditFrom::Network) {
            wantFile(e.file, g_server.guid);
        }
        return;
    }
    // host (or playing alone)
    if (from == EditFrom::Network) {
        auto src = g_fileSource.find(e.file);
        if (src != g_fileSource.end()) wantFile(e.file, src->second);
    }
    for (auto& r : g_peers) sendEdit(r, kEdit, id, e);
}

void netPaintingTick(void* p) {
    std::lock_guard lk(g_mx);
    auto& m = g_isClient ? g_serverEdits : g_pending;
    if (m.empty()) return;
    auto it = m.find(g::entityUniqueId(p));
    if (it == m.end()) return;
    Edit e = it->second, cur;
    if (!g_isClient) m.erase(it);  // host: applied once (later edits of the host win)
    else if (getEdit(p, cur) && cur == e) return;
    setEdit(p, e, EditFrom::Network);
}

void installNetHooks() {
    uint64_t o = 0;
    if (emu_hook(g::addr(g::kRakPeerReceive), (uint64_t)h_receive, &o) == 0) o_receive = reinterpret_cast<ReceiveFn>(o);
    else peLog("cannot hook RakPeer::Receive: multiplayer sync is off");
}
