/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/rtp_rtcp/source/ulpfec_receiver_impl.h"

#include <string.h>

#include <memory>
#include <utility>

#include "absl/memory/memory.h"
#include "api/scoped_refptr.h"
#include "modules/rtp_rtcp/source/byte_io.h"
#include "modules/rtp_rtcp/source/rtp_utility.h"
#include "modules/rtp_rtcp/source/rtp_packet_received.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"

namespace webrtc {

std::unique_ptr<UlpfecReceiver> UlpfecReceiver::Create(
    uint32_t ssrc,
    RecoveredPacketReceiver* callback,
    rtc::ArrayView<const RtpExtension> extensions) {
  return absl::make_unique<UlpfecReceiverImpl>(ssrc, callback, extensions);
}

UlpfecReceiverImpl::UlpfecReceiverImpl(
    uint32_t ssrc,
    RecoveredPacketReceiver* callback,
    rtc::ArrayView<const RtpExtension> extensions)
    : ssrc_(ssrc),
      extensions_(extensions),
      recovered_packet_callback_(callback),
      fec_(ForwardErrorCorrection::CreateUlpfec(ssrc_)) {}

UlpfecReceiverImpl::~UlpfecReceiverImpl() {
  received_packets_.clear();
  fec_->ResetState(&recovered_packets_);
}

FecPacketCounter UlpfecReceiverImpl::GetPacketCounter() const {
  std::unique_lock<std::recursive_mutex> cs((std::recursive_mutex&)crit_sect_);
  return packet_counter_;
}

//     0                   1                    2                   3
//     0 1 2 3 4 5 6 7 8 9 0 1 2 3  4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//    |F|   block PT  |  timestamp offset         |   block length    |
//    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
//
// RFC 2198          RTP Payload for Redundant Audio Data    September 1997
//
//    The bits in the header are specified as follows:
//
//    F: 1 bit First bit in header indicates whether another header block
//        follows.  If 1 further header blocks follow, if 0 this is the
//        last header block.
//        If 0 there is only 1 byte RED header
//
//    block PT: 7 bits RTP payload type for this block.
//
//    timestamp offset:  14 bits Unsigned offset of timestamp of this block
//        relative to timestamp given in RTP header.  The use of an unsigned
//        offset implies that redundant data must be sent after the primary
//        data, and is hence a time to be subtracted from the current
//        timestamp to determine the timestamp of the data for which this
//        block is the redundancy.
//
//    block length:  10 bits Length in bytes of the corresponding data
//        block excluding header.

int32_t UlpfecReceiverImpl::AddReceivedRedPacket(
    const uint8_t* incoming_rtp_packet,
    size_t packet_length,
    uint8_t ulpfec_payload_type) {
  RTPHeader header;
  RtpUtility::RtpHeaderParser parse(incoming_rtp_packet, packet_length);
  parse.Parse(&header);

  if (header.ssrc != ssrc_) {
    RTC_LOG(LS_WARNING) << "Received RED packet with different SSRC than expected; dropping.";
    return -1;
  }
  if (packet_length > IP_PACKET_SIZE) {
    RTC_LOG(LS_WARNING) << "Received RED packet with length exceeds maximum IP "
                           "packet size; dropping.";
    return -1;
  }
  std::unique_lock<decltype(crit_sect_)> cs(crit_sect_);

  uint8_t red_header_length = 1;
  size_t payload_data_length = packet_length - header.headerLength;

  if (payload_data_length == 0) {
    RTC_LOG(LS_WARNING) << "Corrupt/truncated FEC packet.";
    return -1;
  }

  // Remove RED header of incoming packet and store as a virtual RTP packet.
  std::unique_ptr<ForwardErrorCorrection::ReceivedPacket> received_packet(
      new ForwardErrorCorrection::ReceivedPacket());
  received_packet->pkt = new ForwardErrorCorrection::Packet();

  // Get payload type from RED header and sequence number from RTP header.
  uint8_t payload_type = incoming_rtp_packet[header.headerLength] & 0x7f;
  received_packet->is_fec = payload_type == ulpfec_payload_type;
  received_packet->ssrc = header.ssrc;
  received_packet->seq_num = header.sequenceNumber;

  if (incoming_rtp_packet[header.headerLength] & 0x80) {
    // f bit set in RED header, i.e. there are more than one RED header blocks.
    // WebRTC never generates multiple blocks in a RED packet for FEC.
    RTC_LOG(LS_WARNING) << "More than 1 block in RED packet is not supported.";
    return -1;
  }

  ++packet_counter_.num_packets;
  if (packet_counter_.first_packet_time_ms == -1) {
    packet_counter_.first_packet_time_ms = rtc::TimeMillis();
  }

  if (received_packet->is_fec) {
    ++packet_counter_.num_fec_packets;

    // everything behind the RED header
    memcpy(received_packet->pkt->data,
           incoming_rtp_packet + header.headerLength + red_header_length,
           payload_data_length - red_header_length);
    received_packet->pkt->length = payload_data_length - red_header_length;
    received_packet->ssrc =
        ByteReader<uint32_t>::ReadBigEndian(&incoming_rtp_packet[8]);

  } else {
    // Copy RTP header.
    memcpy(received_packet->pkt->data, incoming_rtp_packet,
           header.headerLength);

    // Set payload type.
    received_packet->pkt->data[1] &= 0x80;          // Reset RED payload type.
    received_packet->pkt->data[1] += payload_type;  // Set media payload type.

    // Copy payload data.
    memcpy(received_packet->pkt->data + header.headerLength,
           incoming_rtp_packet + header.headerLength + red_header_length,
           payload_data_length - red_header_length);
    received_packet->pkt->length =
        header.headerLength + payload_data_length - red_header_length;
  }

  if (received_packet->pkt->length == 0) {
    return 0;
  }

  received_packets_.push_back(std::move(received_packet));
  return 0;
}

// TODO(nisse): Drop always-zero return value.
int32_t UlpfecReceiverImpl::ProcessReceivedFec() {
  crit_sect_.lock();

  // If we iterate over |received_packets_| and it contains a packet that cause
  // us to recurse back to this function (for example a RED packet encapsulating
  // a RED packet), then we will recurse forever. To avoid this we swap
  // |received_packets_| with an empty vector so that the next recursive call
  // wont iterate over the same packet again. This also solves the problem of
  // not modifying the vector we are currently iterating over (packets are added
  // in AddReceivedRedPacket).
  std::vector<std::unique_ptr<ForwardErrorCorrection::ReceivedPacket>>
      received_packets;
  received_packets.swap(received_packets_);

  for (const auto& received_packet : received_packets) {
    // Send received media packet to VCM.
    if (!received_packet->is_fec) {
      ForwardErrorCorrection::Packet* packet = received_packet->pkt;
      crit_sect_.unlock();
      recovered_packet_callback_->OnRecoveredPacket(packet->data,
                                                    packet->length);
      crit_sect_.lock();
      RtpPacketReceived rtp_packet;
      // TODO(ilnik): move extension nullifying out of RtpPacket, so there's no
      // need to create one here, and avoid two memcpy calls below.
      rtp_packet.Parse(packet->data, packet->length);  // Does memcopy.
      rtp_packet.IdentifyExtensions(extensions_);
      rtp_packet.CopyAndZeroMutableExtensions(  // Does memcopy.
          rtc::MakeArrayView(packet->data, packet->length));
    }
    fec_->DecodeFec(*received_packet, &recovered_packets_);
  }

  // Send any recovered media packets to VCM.
  for (const auto& recovered_packet : recovered_packets_) {
    if (recovered_packet->returned) {
      // Already sent to the VCM and the jitter buffer.
      continue;
    }
    ForwardErrorCorrection::Packet* packet = recovered_packet->pkt;
    ++packet_counter_.num_recovered_packets;
    // Set this flag first; in case the recovered packet carries a RED
    // header, OnRecoveredPacket will recurse back here.
    recovered_packet->returned = true;
    crit_sect_.unlock();
    recovered_packet_callback_->OnRecoveredPacket(packet->data, packet->length);
    crit_sect_.lock();
  }

  crit_sect_.unlock();
  return 0;
}

}  // namespace webrtc
