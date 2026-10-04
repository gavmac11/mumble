// Test-only C ABI around the actual client queue; never shipped in the product.
#include "VideoPacketPacer.h"
#include <cstdint>
#include <cstring>

using Mumble::Video::PacketQueue;
using Mumble::Video::VideoPackets;

extern "C" {
void *mumble_pilot_pacer_new(std::uint64_t rate) noexcept {
    try { return new PacketQueue(rate); } catch (...) { return nullptr; }
}
void mumble_pilot_pacer_free(void *handle) noexcept {
    delete static_cast<PacketQueue *>(handle);
}
int mumble_pilot_pacer_enqueue(void *handle, const unsigned char *data, unsigned int length,
                              const unsigned int *sizes, unsigned int count, int keyframe,
                              std::uint64_t now) noexcept {
    if (!handle || !data || !sizes || !count || count > PacketQueue::MaximumFramePackets
        || length > PacketQueue::MaximumFramePackets * PacketQueue::MaximumPacketBytes) return -1;
    try {
        VideoPackets packets;
        unsigned int offset = 0;
        for (unsigned int i = 0; i < count; ++i) {
            if (!sizes[i] || sizes[i] > PacketQueue::MaximumPacketBytes || sizes[i] > length - offset) return -1;
            packets.emplace_back(data + offset, data + offset + sizes[i]);
            offset += sizes[i];
        }
        if (offset != length) return -1;
        return static_cast<PacketQueue *>(handle)->enqueue(std::move(packets), keyframe != 0, now) ? 1 : 0;
    } catch (...) { return -1; }
}
int mumble_pilot_pacer_take(void *handle, std::uint64_t now, unsigned char *output,
                           unsigned int capacity) noexcept {
    if (!handle || !output || capacity < PacketQueue::MaximumPacketBytes) return -1;
    try {
        auto packet = static_cast<PacketQueue *>(handle)->takeReady(now);
        if (!packet) return 0;
        std::memcpy(output, packet->data(), packet->size());
        return static_cast<int>(packet->size());
    } catch (...) { return -1; }
}
std::int64_t mumble_pilot_pacer_delay(void *handle, std::uint64_t now) noexcept {
    if (!handle) return -2;
    auto delay = static_cast<PacketQueue *>(handle)->delayNs(now);
    return delay ? static_cast<std::int64_t>(*delay) : -1;
}
}
