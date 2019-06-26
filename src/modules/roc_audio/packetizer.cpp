/*
 * Copyright (c) 2015 Roc authors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "roc_audio/packetizer.h"
#include "roc_core/log.h"
#include "roc_core/panic.h"
#include "roc_core/random.h"

namespace roc {
namespace audio {

Packetizer::Packetizer(packet::IWriter& writer,
                       packet::IComposer& composer,
                       IEncoder& encoder,
                       packet::PacketPool& packet_pool,
                       core::BufferPool<uint8_t>& buffer_pool,
                       packet::channel_mask_t channels,
                       core::nanoseconds_t packet_length,
                       size_t sample_rate,
                       unsigned int payload_type)
    : writer_(writer)
    , composer_(composer)
    , encoder_(encoder)
    , packet_pool_(packet_pool)
    , buffer_pool_(buffer_pool)
    , channels_(channels)
    , num_channels_(packet::num_channels(channels))
    , samples_per_packet_(
          (packet::timestamp_t)packet::timestamp_from_ns(packet_length, sample_rate))
    , payload_type_(payload_type)
    , payload_size_(encoder_.payload_size(samples_per_packet_))
    , packet_pos_(0)
    , source_((packet::source_t)core::random(packet::source_t(-1)))
    , seqnum_((packet::seqnum_t)core::random(packet::seqnum_t(-1)))
    , timestamp_((packet::timestamp_t)core::random(packet::timestamp_t(-1))) {
    roc_log(LogDebug, "packetizer: initializing: n_channels=%lu samples_per_packet=%lu",
            (unsigned long)num_channels_, (unsigned long)samples_per_packet_);
}

void Packetizer::write(Frame& frame) {
    if (frame.size() % num_channels_ != 0) {
        roc_panic("packetizer: unexpected frame size");
    }

    const sample_t* buffer_ptr = frame.data();
    size_t buffer_samples = frame.size() / num_channels_;

    while (buffer_samples != 0) {
        if (!packet_) {
            if (!begin_packet_()) {
                return;
            }
        }

        size_t ns = encoder_.write_samples(*packet_, packet_pos_, buffer_ptr,
                                           buffer_samples, channels_);

        packet_pos_ += ns;
        buffer_samples -= ns;
        buffer_ptr += ns * num_channels_;

        if (packet_pos_ == samples_per_packet_) {
            end_packet_();
        }
    }
}

void Packetizer::flush() {
    if (packet_) {
        end_packet_();
    }
}

bool Packetizer::begin_packet_() {
    packet::PacketPtr pp = create_packet_();
    if (!pp) {
        return false;
    }

    packet::RTP* rtp = pp->rtp();
    if (!rtp) {
        roc_panic("packetizer: unexpected non-rtp packet");
    }

    rtp->source = source_;
    rtp->seqnum = seqnum_;
    rtp->timestamp = timestamp_;
    rtp->payload_type = payload_type_;

    packet_ = pp;

    return true;
}

void Packetizer::end_packet_() {
    if (packet_pos_ < samples_per_packet_) {
        pad_packet_();
    }

    writer_.write(packet_);

    seqnum_++;
    timestamp_ += (packet::timestamp_t)packet_pos_;

    packet_ = NULL;
    packet_pos_ = 0;
}

void Packetizer::pad_packet_() {
    const size_t actual_payload_size = encoder_.payload_size(packet_pos_);
    roc_panic_if_not(actual_payload_size <= payload_size_);

    if (actual_payload_size == payload_size_) {
        return;
    }

    if (!composer_.pad(*packet_, payload_size_ - actual_payload_size)) {
        roc_panic("packetizer: can't pad packet: orig_size=%lu actual_size=%lu",
                  (unsigned long)payload_size_, (unsigned long)actual_payload_size);
    }
}

packet::PacketPtr Packetizer::create_packet_() {
    packet::PacketPtr packet = new (packet_pool_) packet::Packet(packet_pool_);
    if (!packet) {
        roc_log(LogError, "packetizer: can't allocate packet");
        return NULL;
    }

    packet->add_flags(packet::Packet::FlagAudio);

    core::Slice<uint8_t> data = new (buffer_pool_) core::Buffer<uint8_t>(buffer_pool_);
    if (!data) {
        roc_log(LogError, "packetizer: can't allocate buffer");
        return NULL;
    }

    if (!composer_.prepare(*packet, data, payload_size_)) {
        roc_log(LogError, "packetizer: can't prepare packet");
        return NULL;
    }

    packet->set_data(data);

    return packet;
}

} // namespace audio
} // namespace roc
