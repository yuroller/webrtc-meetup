#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_peerconnection_factory.h"
#include "api/video_track_source_proxy.h"
#include "api/data_channel_interface.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "media/engine/internal_decoder_factory.h"
#include "media/engine/internal_encoder_factory.h"
#include "media/engine/multiplex_codec_factory.h"
#include "modules/audio_device/include/audio_device.h"
#include "modules/audio_processing/include/audio_processing.h"
#include "modules/video_capture/video_capture_factory.h"
#include "pc/video_track_source.h"

#if defined(WEBRTC_WIN)
#include "rtc_base/win32_socket_init.h"
#endif

#include <iostream>
#include <utility>

static std::unique_ptr<rtc::Thread> g_worker_thread;
static std::unique_ptr<rtc::Thread> g_signaling_thread;
static rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface>
	g_peer_connection_factory;

static void Init()
{
	g_worker_thread = rtc::Thread::CreateWithSocketServer();
	g_worker_thread->Start();
	g_signaling_thread = rtc::Thread::Create();
	g_signaling_thread->Start();

	g_peer_connection_factory = webrtc::CreatePeerConnectionFactory(
		g_worker_thread.get(), // network thread same as worker thread
		g_worker_thread.get(),
		g_signaling_thread.get(),
		nullptr, // audio device module
		webrtc::CreateBuiltinAudioEncoderFactory(),
		webrtc::CreateBuiltinAudioDecoderFactory(),
		std::make_unique<webrtc::MultiplexEncoderFactory>(
				std::make_unique<webrtc::InternalEncoderFactory>()),
		std::make_unique<webrtc::MultiplexDecoderFactory>(
				std::make_unique<webrtc::InternalDecoderFactory>()),
		nullptr, // audio mixer
		nullptr);  // audio processing
}

static void Destroy()
{
	g_peer_connection_factory = nullptr;
	g_signaling_thread.reset();
	g_worker_thread.reset();
}
namespace
{
	class DummySetSessionDescriptionObserver
		: public webrtc::SetSessionDescriptionObserver {
	public:
		static DummySetSessionDescriptionObserver* Create() {
			return new rtc::RefCountedObject<DummySetSessionDescriptionObserver>();
		}
		void OnSuccess() override { RTC_LOG(INFO) << __FUNCTION__; }
		void OnFailure(webrtc::RTCError error) override {
			RTC_LOG(WARNING) << __FUNCTION__ << " " << ToString(error.type()) << ": "
				<< error.message();
		}
	};

	class MyPeerConnection :
		public webrtc::PeerConnectionObserver,
		public webrtc::CreateSessionDescriptionObserver,
		public webrtc::DataChannelObserver
	{
	public:
		explicit MyPeerConnection(std::string name)
			: name_(std::move(name)),
			sdp_available_(true, false),
			candidates_available_(true, false)
		{}

		bool Init() {
			RTC_DCHECK(g_peer_connection_factory.get() != nullptr);
			RTC_DCHECK(peer_connection_.get() == nullptr);

			webrtc::PeerConnectionInterface::RTCConfiguration config;
			//config.enable_dtls_srtp = false;
			//config.enable_rtp_data_channel = true;
			// stun server
			//webrtc::PeerConnectionInterface::IceServer stun_server;
			//stun_server.uri = "stun:stun.l.google.com:19302";
			//config.servers.push_back(stun_server);

			peer_connection_ = g_peer_connection_factory->CreatePeerConnection(
				config, nullptr, nullptr, this);

			return peer_connection_.get() != nullptr;
		}

		void Destroy() {
			CloseDataChannel();
			peer_connection_ = nullptr;
		}

		bool CreateOffer() {
			if (!peer_connection_.get()) {
				return false;
			}

			webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
			options.offer_to_receive_audio = 0;
			options.offer_to_receive_video = 0;
			peer_connection_->CreateOffer(this, options);
			return true;
		}

		bool CreateAnswer() {
			if (!peer_connection_.get()) {
				return false;
			}

			webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
			options.offer_to_receive_audio = 0;
			options.offer_to_receive_video = 0;
			peer_connection_->CreateAnswer(this, options);
			return true;
		}

		bool AddIceCandidates(
			const std::vector<std::unique_ptr<webrtc::IceCandidateInterface>>* candidates) {
			if (candidates == nullptr) {
				return false;
			}

			for (const auto& c : *candidates) {
				if (!peer_connection_->AddIceCandidate(c.get())) {
					std::cerr << "Error adding candidate\n";
					return false;
				}
			}

			return true;
		}

		bool CreateDataChannel() {
			struct webrtc::DataChannelInit init;
			data_channel_ = peer_connection_->CreateDataChannel("Hello", &init);
			if (!data_channel_.get()) {
				return false;
			}

			data_channel_->RegisterObserver(this);
			return true;
		}

		void CloseDataChannel() {
			if (data_channel_.get()) {
				data_channel_->UnregisterObserver();
				data_channel_->Close();
			}

			data_channel_ = nullptr;
		}

		bool SendViaDataChannel(const std::string& data) {
			if (!data_channel_.get()) {
				return false;
			}

			webrtc::DataBuffer buffer(data);
			data_channel_->Send(buffer);
			return true;
		}

		std::string WaitSdp(int waitMs) {
			if (!sdp_available_.Wait(waitMs)) {
				return std::string();
			}

			return sdp_description_;
		}

		const std::vector<std::unique_ptr<webrtc::IceCandidateInterface>>*
			WaitCandidates(int waitMs) {
			if (!candidates_available_.Wait(waitMs)) {
				return nullptr;
			}

			return &candidates_;
		}

		bool SetRemoteDescription(const std::string& sdpType,
			const std::string& sdpDescription) {
			if (!peer_connection_.get()) {
				return false;
			}

			webrtc::SdpParseError error;
			webrtc::SessionDescriptionInterface* session_description =
				webrtc::CreateSessionDescription(
					sdpType, sdpDescription, &error);
			if (!session_description) {
				std::cerr << "Can't parse received session description message. "
					<< "SdpParseError was: " << error.description;
				return false;
			}
			
			std::cout << "Received session description :" << sdpDescription;
			peer_connection_->SetRemoteDescription(
				DummySetSessionDescriptionObserver::Create(), session_description);
			return true;
		}

	protected:
		// PeerConnectionObserver implementation.
		void OnSignalingChange(
			webrtc::PeerConnectionInterface::SignalingState new_state) override {}
		void OnAddStream(
			rtc::scoped_refptr<webrtc::MediaStreamInterface> stream) override {}
		void OnRemoveStream(
			rtc::scoped_refptr<webrtc::MediaStreamInterface> stream) override {}

		void OnDataChannel(
			rtc::scoped_refptr<webrtc::DataChannelInterface> channel) override {
			channel->RegisterObserver(this);
			data_channel_ = channel;
		}

		void OnRenegotiationNeeded() override {}
		void OnIceConnectionChange(
			webrtc::PeerConnectionInterface::IceConnectionState new_state) override {}
		
		void OnIceGatheringChange(
			webrtc::PeerConnectionInterface::IceGatheringState new_state) override {
			if (new_state == webrtc::PeerConnectionInterface::IceGatheringState::kIceGatheringComplete) {
				candidates_available_.Set();
			}
		}

		void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
			std::string sdp;
			if (!candidate->ToString(&sdp)) {
				std::cerr << "Failed to serialize candidate\n";
				return;
			}

			std::cout << "OnIceCandidate: " << sdp << '\n';

			candidates_.emplace_back(webrtc::CreateIceCandidate(
				candidate->sdp_mid(),
				candidate->sdp_mline_index(),
				candidate->candidate()
			));
		}

		void OnIceConnectionReceivingChange(bool receiving) override {}

		// CreateSessionDescriptionObserver implementation.
		void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
			std::cout << "CreateSessionDescription->OnSuccess\n";
			peer_connection_->SetLocalDescription(
				DummySetSessionDescriptionObserver::Create(), desc);
			if (desc->ToString(&sdp_description_)) {
				std::cout << sdp_description_ << '\n';
				sdp_available_.Set();
			}
		}

		void OnFailure(webrtc::RTCError error) override {
			std::cerr << "CreateSessionDescription->OnFailure\n";
		}

		// DataChannelObserver implementation.
		void OnStateChange() override {
			if (data_channel_) {
				webrtc::DataChannelInterface::DataState state = data_channel_->state();
				if (state == webrtc::DataChannelInterface::kOpen) {					
					std::cout << "Data channel is open (id=" << data_channel_ <<")\n";
				}
			}
		}

		void OnMessage(const webrtc::DataBuffer& buffer) override {
			if (buffer.binary) {
				std::cout << "Binary message receveid of len=" << buffer.data.size() << "\n";
			}
			else {
				std::string text(buffer.data.data<char>(), buffer.data.size());
				std::cout << "Text message received='" << text << "'\n";
			}
		}

	private:
		std::string name_;
		rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection_;
		rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel_;
		rtc::Event sdp_available_;
		std::string sdp_description_;
		rtc::Event candidates_available_;
		std::vector<std::unique_ptr<webrtc::IceCandidateInterface>> candidates_;
	};
}

static rtc::scoped_refptr<MyPeerConnection> CreateCaller()
{
	rtc::scoped_refptr<MyPeerConnection> caller
		= new rtc::RefCountedObject<MyPeerConnection>("caller");
	if (!caller->Init()) {
		std::cerr << "Error init pc\n";
		return nullptr;
	}

	if (!caller->CreateDataChannel()) {
		std::cerr << "Error Create Data Channel\n";
		return nullptr;
	}

	if (!caller->CreateOffer()) {
		std::cerr << "Error Create Offer\n";
		return nullptr;
	}

	std::string offerSdp(caller->WaitSdp(1000));
	if (offerSdp.empty()) {
		std::cerr << "No sdp set\n";
		return nullptr;
	}

	return caller;
}

static rtc::scoped_refptr<MyPeerConnection> CreateReceiver(const std::string& sdp,
	const std::vector<std::unique_ptr<webrtc::IceCandidateInterface>>* candidates)
{
	rtc::scoped_refptr<MyPeerConnection> receiver
		= new rtc::RefCountedObject<MyPeerConnection>("receiver");
	if (!receiver->Init()) {
		std::cerr << "Error init pc\n";
		return nullptr;
	}

	//if (!receiver->CreateDataChannel()) {
	//	std::cerr << "Error Create Data Channel\n";
	//	return nullptr;
	//}

	if (!receiver->SetRemoteDescription(
		webrtc::SessionDescriptionInterface::kOffer, sdp)) {
		std::cerr << "Error Set Remote Description\n";
		return nullptr;
	}

	if (!receiver->AddIceCandidates(candidates)) {
		std::cerr << "Error Set Remote Candidates\n";
	}

	if (!receiver->CreateAnswer()) {
		std::cerr << "Error Create Answer\n";
		return nullptr;
	}

	std::string answerSdp(receiver->WaitSdp(1000));
	if (answerSdp.empty()) {
		std::cerr << "No sdp set\n";
		return nullptr;
	}

	return receiver;
}

int main()
{
#if defined(WEBRTC_WIN)
	rtc::WinsockInitializer winsock_init;
#endif

	Init();
	rtc::scoped_refptr<MyPeerConnection> caller = CreateCaller();
	std::string offerSdp = caller->WaitSdp(3000);
	auto caller_candidates = caller->WaitCandidates(30000);

	rtc::scoped_refptr<MyPeerConnection> receiver
		= CreateReceiver(offerSdp, caller_candidates);
	std::string answerSdp = receiver->WaitSdp(3000);

	if (!caller->SetRemoteDescription(
		webrtc::SessionDescriptionInterface::kAnswer, answerSdp)) {
		std::cerr << "Unable to set remote sdp on caller\n";
	}

	auto receiver_candidates = receiver->WaitCandidates(30000);
	if (!caller->AddIceCandidates(receiver_candidates))	{
		std::cerr << "Unable to set receiver candidates\n";
	}

	rtc::Thread::Current()->SleepMs(1000);
	if (!caller->SendViaDataChannel("Message from caller")) {
		std::cerr << "Unable to send message from caller\n";
	}

	rtc::Thread::Current()->SleepMs(1000);
	if (!receiver->SendViaDataChannel("Message from receiver")) {
		std::cerr << "Unable to send message from receiver\n";
	}

	rtc::Thread::Current()->SleepMs(3000);
	caller->Destroy();
	receiver->Destroy();
	Destroy();

	return 0;
}
