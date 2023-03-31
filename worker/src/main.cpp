#define MS_CLASS "mediasoup-worker"
// #define MS_LOG_DEV_LEVEL 3

#include "MediaSoupErrors.hpp"
#include "lib.hpp"
#include <cstdlib> // std::_Exit(), std::genenv()
#include <string>
#include "modules/rtp_rtcp/include/flexfec_receiver.h"
#include "modules/rtp_rtcp/include/flexfec_sender.h"
#include "modules/rtp_rtcp/source/rtp_packet_to_send.h"
#include "system_wrappers/include/clock.h"
using namespace webrtc;
class test : RecoveredPacketReceiver {
    std::auto_ptr<FlexfecReceiver> receiver_;
    std::auto_ptr<FlexfecSender>  sender_;
    std::map<uint16_t, RtpPacketReceived> record_map;
public:
    test() {
        std::vector<RtpExtension> vec;
        rtc::ArrayView<const RtpExtensionSize> size;
        std::string mid("mytest");

        Clock *clock = Clock::GetRealTimeClock();
        sender_.reset(new FlexfecSender(113, 123123, 123123, mid, vec, size, nullptr, clock));

        FecProtectionParams delta_params;
        delta_params.fec_rate = 255;
        delta_params.max_fec_frames = 10;
        delta_params.fec_mask_type = kFecMaskRandom;
        sender_->SetFecParameters(delta_params);

        FecProtectionParams key_params;
        key_params.fec_rate = 255;
        key_params.max_fec_frames = 10;
        key_params.fec_mask_type = kFecMaskRandom;
        //sender_->SetProtectionParameters(delta_params, key_params);

        receiver_.reset(new FlexfecReceiver(123123, 123123, this));
    }
    ~test() {
    }

    void OnRecoveredPacket(const uint8_t *packet, size_t length) override {
        webrtc::RtpPacketReceived parsed_packet(nullptr);
        if (!parsed_packet.Parse(packet, length)) {
            std::cout << "parse err" << std::endl;
            return;
        }
        record_map[parsed_packet.SequenceNumber()] = parsed_packet;
    }

    void WorkTest() {
        FILE *origin = fopen("../res/test.h264", "r");
        if (!origin) {
            return;
        }

        for (uint16_t i = 0; i < 65535; i++) {
            char s[1000];
            fgets(s, 1000, origin);

            webrtc::RtpPacketToSend pkt(nullptr, 1000);
            //pkt.Parse(kPacketWithH264, sizeof(kPacketWithH264));
            pkt.SetSequenceNumber(i);
            pkt.SetPayloadType(101);
            pkt.SetSsrc(123123);
            memcpy(pkt.Buffer().data(), s, 1000);
            pkt.SetPayloadSize(1000);
            if (i % 48 == 0)
                pkt.SetMarker(true);

            sender_->AddRtpPacketAndGenerateFec(pkt);

            auto vec_s = sender_->GetFecPackets();
            if (vec_s.size()) {
                for (auto iter = vec_s.begin(); iter != vec_s.end(); iter++) {
                    webrtc::RtpPacketReceived parsed_packet(nullptr);
                    parsed_packet.Parse((*iter)->data(), (*iter)->size());
                    parsed_packet.SetPayloadType(113);
                    receiver_->OnRtpPacket(parsed_packet);
                }
            }

            {
                if (i % 2 == 0) {
                    continue;
                }
                webrtc::RtpPacketReceived parsed_packet(nullptr);
                parsed_packet.Parse(pkt.data(), pkt.size());
                receiver_->OnRtpPacket(parsed_packet);
                record_map[parsed_packet.SequenceNumber()] = parsed_packet;
            }
        }

        fclose(origin);
    }
};

static constexpr int ConsumerChannelFd{ 3 };
static constexpr int ProducerChannelFd{ 4 };
static constexpr int PayloadConsumerChannelFd{ 5 };
static constexpr int PayloadProducerChannelFd{ 6 };

int main(int argc, char* argv[])
{
    test t; t.WorkTest();
	// Ensure we are called by our Node library.
	if (!std::getenv("MEDIASOUP_VERSION"))
	{
		MS_ERROR_STD("you don't seem to be my real father!");

		std::_Exit(EXIT_FAILURE);
	}

	const std::string version = std::getenv("MEDIASOUP_VERSION");

	auto statusCode = mediasoup_worker_run(
	  argc,
	  argv,
	  version.c_str(),
	  ConsumerChannelFd,
	  ProducerChannelFd,
	  PayloadConsumerChannelFd,
	  PayloadProducerChannelFd,
	  nullptr,
	  nullptr,
	  nullptr,
	  nullptr,
	  nullptr,
	  nullptr,
	  nullptr,
	  nullptr);

	switch (statusCode)
	{
		case 0:
			std::_Exit(EXIT_SUCCESS);
		case 1:
			std::_Exit(EXIT_FAILURE);
		case 42:
			std::_Exit(42);
	}
}
