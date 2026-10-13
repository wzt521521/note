// ========== 1. 协议定义 + Packet 编解码 (6.1) ==========
// | length 4B | command 2B | payload length B |   header 固定 6 字节
constexpr uint32_t kHeaderSize = 6;

struct Packet {
    uint16_t command;
    std::string payload;

    std::vector<uint8_t> encode() const {
        uint32_t len = payload.size();
        std::vector<uint8_t> buf(6 + len);
        memcpy(buf.data(), &len, 4);                      // 6.8 套 htonl
        memcpy(buf.data() + 4, &command, 2);              // 6.8 套 htons
        memcpy(buf.data() + 6, payload.data(), len);
        return buf;
    }
};

// 只看 header 拿 length; 再按 length 还原一个包(调用前确保数据已够)
static uint32_t peekLength(const uint8_t* p) {
    uint32_t len; memcpy(&len, p, 4); return len;         // 6.8 套 ntohl
}
static Packet decodePacket(const uint8_t* p) {
    uint32_t len = peekLength(p);
    Packet pk;
    memcpy(&pk.command, p + 4, 2);
    pk.payload.assign((const char*)(p + 6), len);
    return pk;
}

// ========== 2. InputBuffer: 拆包核心 (6.4/6.5) ==========
// readIdx~writeIdx 之间是已 recv 未处理的字节, 半包残留就活在这里
class InputBuffer {
public:
    void append(const uint8_t* d, size_t n) {             // recv 到就追加
        if (readIdx_ > 0) { /* 空间不够时 memmove 压缩, 略 */ }
        buf_.insert(buf_.end(), d, d + n);
    }

    bool popPacket(Packet* out) {
        size_t avail = buf_.size() - readIdx_;
        if (avail < 6) return false;                      // header 都不齐
        uint32_t len = peekLength(buf_.data() + readIdx_);
        if (avail < 6 + len) return false;                // body 不齐: 半包
        *out = decodePacket(buf_.data() + readIdx_);
        readIdx_ += 6 + len;                              // 取走, 只移下标
        return true;
    }
private:
    std::vector<uint8_t> buf_;
    size_t readIdx_ = 0;
};

// ========== 3. server 收包主循环: 永远长这样 ==========
while (true) {
    ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
    if (n == 0) break;                    // FIN
    in.append(tmp, n);                    // ① 新字节入库
    Packet pk;
    while (in.popPacket(&pk)) {           // ② 能拆几个拆几个(粘包)
        handlePacket(pk);                 // ③ 业务处理, 拆不出的留在 buffer
    }
}

// ========== 4. OutputBuffer: 循环发送核心 (6.6) ==========
class OutputBuffer {
public:
    void append(const void* d, size_t n) {
        buf_.insert(buf_.end(), (const uint8_t*)d, (const uint8_t*)d + n);
    }
    bool empty() const { return readIdx_ == buf_.size(); }

    bool writeToFd(int fd) {
        while (readIdx_ < buf_.size()) {
            ssize_t n = send(fd, buf_.data() + readIdx_,
                             buf_.size() - readIdx_, MSG_NOSIGNAL);
            if (n > 0) { readIdx_ += n; continue; }       // 短写: 继续发剩余
            if (errno == EINTR) continue;
            if (errno == EAGAIN) return false;            // 满了: 残留保留
            throw "connection error";
        }
        return true;
    }
private:
    std::vector<uint8_t> buf_;
    size_t readIdx_ = 0;               // 已发/未发的分界, 即断点
};

// ========== 5. client 发送骨架 ==========
fcntl(fd, F_SETFL, O_NONBLOCK);       // 非阻塞是前提

OutputBuffer out;
for (auto& pk : packets) {
    auto b = pk.encode();
    out.append(b.data(), b.size());   // 业务只管往里塞, 不等 send
}

while (!out.empty()) {
    if (out.writeToFd(fd)) break;     // 全发完
    // EAGAIN: 真实服务器注册 EPOLLOUT 等通知; 骨架里示意等待
    waitWritable(fd);
}
shutdown(fd, SHUT_WR);               // 必须 buffer 清空才能 FIN
