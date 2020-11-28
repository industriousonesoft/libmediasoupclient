#define MSC_CLASS "Handler"

#include "Handler.hpp"
#include "Logger.hpp"
#include "MediaSoupClientErrors.hpp"
#include "PeerConnection.hpp"
#include "ortc.hpp"
#include "sdptransform.hpp"
#include "sdp/Utils.hpp"
#include <cinttypes> // PRIu64, etc

using json = nlohmann::json;

constexpr uint16_t SctpNumStreamsOs{ 1024u };
constexpr uint16_t SctpNumStreamsMis{ 1024u };

json SctpNumStreams = { { "OS", SctpNumStreamsOs }, { "MIS", SctpNumStreamsMis } };

// Static functions declaration.
static void fillJsonRtpEncodingParameters(
  json& jsonEncoding, const webrtc::RtpEncodingParameters& encoding);

namespace mediasoupclient
{
	/* Handler static methods. */

	json Handler::GetNativeRtpCapabilities(const PeerConnection::Options* peerConnectionOptions)
	{
		MSC_TRACE();

		std::unique_ptr<PeerConnection::PrivateListener> privateListener(
		  new PeerConnection::PrivateListener());
		std::unique_ptr<PeerConnection> pc(
		  new PeerConnection(privateListener.get(), peerConnectionOptions));

		(void)pc->AddTransceiver(cricket::MediaType::MEDIA_TYPE_AUDIO);
		(void)pc->AddTransceiver(cricket::MediaType::MEDIA_TYPE_VIDEO);

		webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;

		// May throw.
		auto offer                 = pc->CreateOffer(options);
		auto sdpObject             = sdptransform::parse(offer);
		auto nativeRtpCapabilities = Sdp::Utils::extractRtpCapabilities(sdpObject);

		return nativeRtpCapabilities;
	}

	json Handler::GetNativeSctpCapabilities()
	{
		MSC_TRACE();

		json caps = { { "numStreams", SctpNumStreams } };

		return caps;
	}

	/* Handler instance methods. */

	Handler::Handler(
	  PrivateListener* privateListener,
	  const json& iceParameters,
	  const json& iceCandidates,
	  const json& dtlsParameters,
	  const json& sctpParameters,
	  const PeerConnection::Options* peerConnectionOptions)
	  : privateListener(privateListener)
	{
		MSC_TRACE();

		this->pc.reset(new PeerConnection(this, peerConnectionOptions));

		this->remoteSdp.reset(
		  new Sdp::RemoteSdp(iceParameters, iceCandidates, dtlsParameters, sctpParameters));
	};

	void Handler::Close()
	{
		MSC_TRACE();

		this->pc->Close();
	};

	json Handler::GetTransportStats()
	{
		MSC_TRACE();

		return this->pc->GetStats();
	}

	void Handler::UpdateIceServers(const json& iceServerUris)
	{
		MSC_TRACE();

		auto configuration = this->pc->GetConfiguration();

		configuration.servers.clear();

		for (const auto& iceServerUri : iceServerUris)
		{
			webrtc::PeerConnectionInterface::IceServer iceServer;

			iceServer.uri = iceServerUri;
			configuration.servers.push_back(iceServer);
		}

		if (this->pc->SetConfiguration(configuration))
			return;

		MSC_THROW_ERROR("failed to update ICE servers");
	};

	void Handler::OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState newState)
	{
		MSC_TRACE();

		return this->privateListener->OnConnectionStateChange(newState);
	}

	void Handler::SetupTransport(const std::string& localDtlsRole, json& localSdpObject)
	{
		MSC_TRACE();

		if (localSdpObject.empty())
			localSdpObject = sdptransform::parse(this->pc->GetLocalDescription());

		// Get our local DTLS parameters.
		auto dtlsParameters = Sdp::Utils::extractDtlsParameters(localSdpObject);

		// Set our DTLS role.
		dtlsParameters["role"] = localDtlsRole;

		// Update the remote DTLS role in the SDP.
		std::string remoteDtlsRole = localDtlsRole == "client" ? "server" : "client";
		this->remoteSdp->UpdateDtlsRole(remoteDtlsRole);

		// May throw.
		this->privateListener->OnConnect(dtlsParameters);
		this->transportReady = true;
	};

	/* SendHandler instance methods. */

	SendHandler::SendHandler(
	  Handler::PrivateListener* privateListener,
	  const json& iceParameters,
	  const json& iceCandidates,
	  const json& dtlsParameters,
	  const json& sctpParameters,
	  const PeerConnection::Options* peerConnectionOptions,
	  const json& sendingRtpParametersByKind,
	  const json& sendingRemoteRtpParametersByKind)
	  : Handler(
	      privateListener, iceParameters, iceCandidates, dtlsParameters, sctpParameters, peerConnectionOptions)
	{
		MSC_TRACE();

		this->sendingRtpParametersByKind = sendingRtpParametersByKind;

		this->sendingRemoteRtpParametersByKind = sendingRemoteRtpParametersByKind;
	};

	SendHandler::SendResult SendHandler::Send(
	  webrtc::MediaStreamTrackInterface* track,
	  std::vector<webrtc::RtpEncodingParameters>* encodings,
	  const json* codecOptions)
	{
		MSC_TRACE();

		// Check if the track is a null pointer.
		if (!track)
			MSC_THROW_TYPE_ERROR("missing track");

		MSC_DEBUG("[kind:%s, track->id():%s]", track->kind().c_str(), track->id().c_str());

		if (encodings && encodings->size() > 1)
		{
			uint8_t idx = 0;
			//编码参数集合按顺序编号，从0开始
			for (webrtc::RtpEncodingParameters& encoding : *encodings)
			{
				encoding.rid = std::string("r").append(std::to_string(idx++));
			}
		}
		//获得下一个media section idx
		const Sdp::RemoteSdp::MediaSectionIdx mediaSectionIdx = this->remoteSdp->GetNextMediaSectionIdx();

		webrtc::RtpTransceiverInit transceiverInit;

		if (encodings && !encodings->empty())
			transceiverInit.send_encodings = *encodings;

		webrtc::RtpTransceiverInterface* transceiver = this->pc->AddTransceiver(track, transceiverInit);

		if (!transceiver)
			MSC_THROW_ERROR("error creating transceiver");
		// send端的transceiver只发送不接收
		transceiver->SetDirectionWithError(webrtc::RtpTransceiverDirection::kSendOnly);

		std::string offer;
		std::string localId;
		//指定track类型对应的RTP parameters，包括rtp协议簇、编码参数集
		json& sendingRtpParameters = this->sendingRtpParametersByKind[track->kind()];

		try
		{
			webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;

			offer               = this->pc->CreateOffer(options);
			auto localSdpObject = sdptransform::parse(offer);

			// Transport is not ready.
			//由于在SFU模式下，远端SFU服务器将自己伪装成peer。所以相对于当前的数据发送端而言，远端服务器的角色则是数据接收端client，因此设置发送端的角色则是server(音视频数据的server)
			if (!this->transportReady)
				// sserver表示是DTLS连接的被动接收端，反正client端则是DTLS连接的主动建立端
				this->SetupTransport("server", localSdpObject);

			MSC_DEBUG("calling pc->SetLocalDescription():\n%s", offer.c_str());

			this->pc->SetLocalDescription(PeerConnection::SdpType::OFFER, offer);

			// We can now get the transceiver.mid.
			localId = transceiver->mid().value();

			// Set MID.
			sendingRtpParameters["mid"] = localId;
		}
		catch (std::exception& error)
		{
			// Panic here. Try to undo things.
			transceiver->SetDirectionWithError(webrtc::RtpTransceiverDirection::kInactive);
			transceiver->sender()->SetTrack(nullptr);

			throw;
		}

		auto localSdp = this->pc->GetLocalDescription();
		// FIXME:SDP字符串转对象化
		auto localSdpObject = sdptransform::parse(localSdp);

		// media流参数合集
		json& offerMediaObject = localSdpObject["media"][mediaSectionIdx.idx];

		// Set RTCP CNAME.
		sendingRtpParameters["rtcp"]["cname"] = Sdp::Utils::getCname(offerMediaObject);

		// Set RTP encodings by parsing the SDP offer if no encodings are given.
		if (encodings == nullptr || encodings->empty())
		{
			sendingRtpParameters["encodings"] = Sdp::Utils::getRtpEncodings(offerMediaObject);
		}
		// Set RTP encodings by parsing the SDP offer and complete them with given
		// one if just a single encoding has been given.
		else if (encodings->size() == 1)
		{
			auto newEncodings = Sdp::Utils::getRtpEncodings(offerMediaObject);

			fillJsonRtpEncodingParameters(newEncodings.front(), encodings->front());
			sendingRtpParameters["encodings"] = newEncodings;
		}

		// Otherwise if more than 1 encoding are given use them verbatim.
		else
		{
			sendingRtpParameters["encodings"] = json::array();

			for (const auto& encoding : *encodings)
			{
				json jsonEncoding = {};

				fillJsonRtpEncodingParameters(jsonEncoding, encoding);
				sendingRtpParameters["encodings"].push_back(jsonEncoding);
			}
		}

		// If VP8 and there is effective simulcast, add scalabilityMode to each encoding.
		auto mimeType = sendingRtpParameters["codecs"][0]["mimeType"].get<std::string>();
		//将mimeType按逐个字符改为小写
		std::transform(mimeType.begin(), mimeType.end(), mimeType.begin(), ::tolower);

		// clang-format off
		//encodings的数量大于1意味着启用了simulcast
		//simulcast模式下，将vp8和h264编码改用svc模式，因为二者暂不支持simulcast(似乎最新的webrtc版本已经支持:https://blog.csdn.net/sandfox/article/details/84315457)，
		//且vp8只支持时间可适性（temporal scalability），不支持空间可适性（spatial scalability），所以使用S1T3(应该是等价于L1T3)，详细参见:https://www.w3.org/TR/webrtc-svc/
		if (
			sendingRtpParameters["encodings"].size() > 1 &&
			(mimeType == "video/vp8" || mimeType == "video/h264")
		)
		// clang-format on
		{
			for (auto& encoding : sendingRtpParameters["encodings"])
			{
				encoding["scalabilityMode"] = "S1T3";
			}
		}

		// mediasoup的服务端并没有直接发送remote sdp，而是发送remote rtp
		// parameters。通过remoteSdp与本地offer编码参数协商，最终得到webrtc sdp格式的answer
		this->remoteSdp->CreateAnswer(
		  offerMediaObject,
		  mediaSectionIdx.reuseMid /* 引用传参，如果为空则新建 */,
		  sendingRtpParameters,
		  this->sendingRemoteRtpParametersByKind[track->kind()],
		  codecOptions);

		auto answer = this->remoteSdp->GetSdp();

		MSC_DEBUG("calling pc->SetRemoteDescription():\n%s", answer.c_str());

		this->pc->SetRemoteDescription(PeerConnection::SdpType::ANSWER, answer);

		// Store in the map.
		this->mapMidTransceiver[localId] = transceiver;

		SendResult sendResult;

		sendResult.localId       = localId;
		sendResult.rtpSender     = transceiver->sender();
		sendResult.rtpParameters = sendingRtpParameters;

		return sendResult;
	}

	Handler::DataChannel SendHandler::SendDataChannel(
	  const std::string& label, webrtc::DataChannelInit dataChannelInit)
	{
		MSC_TRACE();

		// DataChannel采用的是sctp协议，一种面向消息（以字节为单位，而非bit）的协议，提供了可靠、高效、有序的数据传输协议，同tcp和udp类似，都是基于IP协议，详见：https://tools.ietf.org/html/rfc4960
		//获得下一个可用的stream id
		uint16_t streamId = this->nextSendSctpStreamId;
		// stcp连接过程中需要协商
		dataChannelInit.negotiated = true;
		dataChannelInit.id         = streamId;

		/* clang-format off */
		json sctpStreamParameters =
		{
			{ "streamId", streamId                  },
			//设置数据是顺序还是乱序
			{ "ordered",  dataChannelInit.ordered   },
			//FIXME: 使用的子协议？
			{ "protocol", dataChannelInit.protocol  }
		};
		/* clang-format on */
		//丢包之前重发的时效性的上限值，超过这个时限将视为丢包，不再重发
		if (dataChannelInit.maxRetransmitTime.has_value())
		{
			// In milliseconds
			sctpStreamParameters["maxPacketLifeTime"] = dataChannelInit.maxRetransmitTime.value();
		}
		//丢包之前重发次数的最大值
		if (dataChannelInit.maxRetransmits.has_value())
		{
			sctpStreamParameters["maxRetransmits"] = dataChannelInit.maxRetransmits.value();
		}

		// This will fill sctpStreamParameters's missing fields with default values.
		//验证sctpStreamParameters的有效性，用缺省值补全未设置的字段
		ortc::validateSctpStreamParameters(sctpStreamParameters);

		// WebRTC创建DataChannel
		rtc::scoped_refptr<webrtc::DataChannelInterface> webrtcDataChannel =
		  this->pc->CreateDataChannel(label, &dataChannelInit);

		// Increase next id.
		this->nextSendSctpStreamId = (this->nextSendSctpStreamId + 1) % SctpNumStreamsMis;

		// If this is the first DataChannel we need to create the SDP answer with
		// m=application section.
		//创建data channel对应的SDP answer application section
		if (!this->hasDataChannelMediaSection)
		{
			webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
			//获取webrtc offer sdp
			std::string offer = this->pc->CreateOffer(options);
			// sdp字符串字典对象化
			auto localSdpObject = sdptransform::parse(offer);
			// application section也属于media section的一种
			const Sdp::RemoteSdp::MediaSectionIdx mediaSectionIdx =
			  this->remoteSdp->GetNextMediaSectionIdx();
			//获取webrtc offer sdp中的application section
			auto offerMediaObject =
			  find_if(localSdpObject["media"].begin(), localSdpObject["media"].end(), [](const json& m) {
				  return m.at("type").get<std::string>() == "application";
			  });

			//如果offer sdp中没有application section说明offer端不支持data channel
			if (offerMediaObject == localSdpObject["media"].end())
			{
				MSC_THROW_ERROR("Missing 'application' media section in SDP offer");
			}

			//如果offer端只有data channel，则需要先以server的身份建立transport连接
			//如果offer端有audio或video section则可能已经建立好transport连接
			if (!this->transportReady)
			{
				this->SetupTransport("server", localSdpObject);
			}

			MSC_DEBUG("calling pc.setLocalDescription() [offer:%s]", offer.c_str());

			this->pc->SetLocalDescription(PeerConnection::SdpType::OFFER, offer);
			// Answer sdp关联offer sdp中的application section
			this->remoteSdp->SendSctpAssociation(*offerMediaObject);
			//获取更新后的anser sdp
			auto sdpAnswer = this->remoteSdp->GetSdp();

			MSC_DEBUG("calling pc.setRemoteDescription() [answer:%s]", sdpAnswer.c_str());
			//设置Answer sdp，WebRTC中开始建立sctp连接
			this->pc->SetRemoteDescription(PeerConnection::SdpType::ANSWER, sdpAnswer);
			this->hasDataChannelMediaSection = true;
		}

		SendHandler::DataChannel dataChannel;

		dataChannel.localId              = std::to_string(streamId);
		dataChannel.dataChannel          = webrtcDataChannel;
		dataChannel.sctpStreamParameters = sctpStreamParameters;

		return dataChannel;
	}

	//停止localId对应的发送端
	void SendHandler::StopSending(const std::string& localId)
	{
		MSC_TRACE();

		MSC_DEBUG("[localId:%s]", localId.c_str());

		auto locaIdIt = this->mapMidTransceiver.find(localId);

		if (locaIdIt == this->mapMidTransceiver.end())
			MSC_THROW_ERROR("associated RtpTransceiver not found");

		// locaIdIt->first是localId
		auto* transceiver = locaIdIt->second;

		// transceiver清空发送端的（音频或视频）数据源(MediaStreamTrackInterface)
		transceiver->sender()->SetTrack(nullptr);
		// pc移除发送track(RtpSenderInterface)
		this->pc->RemoveTrack(transceiver->sender());
		//远端SPD关闭对应的media sction
		this->remoteSdp->CloseMediaSection(transceiver->mid().value());

		// FIXME:
		// 在pc移除sender后，获得更新后的offer，此时的offer已经不具备localId对应的media的发送能力，因此在SetLocalDescription时，对应的底层udp
		// transports也会被移除?
		// May throw.
		webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
		auto offer = this->pc->CreateOffer(options);

		MSC_DEBUG("calling pc->SetLocalDescription():\n%s", offer.c_str());

		// May throw.
		this->pc->SetLocalDescription(PeerConnection::SdpType::OFFER, offer);

		auto localSdpObj = sdptransform::parse(this->pc->GetLocalDescription());
		auto answer      = this->remoteSdp->GetSdp();

		MSC_DEBUG("calling pc->SetRemoteDescription():\n%s", answer.c_str());

		// May throw.
		//同理更新answer sdp，更新本地与远端之间的底层连接
		this->pc->SetRemoteDescription(PeerConnection::SdpType::ANSWER, answer);
	}

	//更新localId对应的transceiver的数据源
	void SendHandler::ReplaceTrack(const std::string& localId, webrtc::MediaStreamTrackInterface* track)
	{
		MSC_TRACE();

		MSC_DEBUG(
		  "[localId:%s, track->id():%s]",
		  localId.c_str(),
		  track == nullptr ? "nullptr" : track->id().c_str());

		auto localIdIt = this->mapMidTransceiver.find(localId);

		if (localIdIt == this->mapMidTransceiver.end())
			MSC_THROW_ERROR("associated RtpTransceiver not found");

		auto* transceiver = localIdIt->second;

		transceiver->sender()->SetTrack(track);
	}

	//设置localId对应的transceiver可支持最大的空间层数，用于simulcast中
	void SendHandler::SetMaxSpatialLayer(const std::string& localId, uint8_t spatialLayer)
	{
		MSC_TRACE();

		MSC_DEBUG("[localId:%s, spatialLayer:%" PRIu8 "]", localId.c_str(), spatialLayer);

		auto localIdIt = this->mapMidTransceiver.find(localId);

		if (localIdIt == this->mapMidTransceiver.end())
			MSC_THROW_ERROR("associated RtpTransceiver not found");

		auto* transceiver = localIdIt->second;
		// RtpParameters，详见：https://webrtc.googlesource.com/src/+/master/api/rtp_parameters.h
		auto parameters = transceiver->sender()->GetParameters();

		bool hasLowEncoding{ false };
		bool hasMediumEncoding{ false };
		bool hasHighEncoding{ false };
		webrtc::RtpEncodingParameters* lowEncoding{ nullptr };
		webrtc::RtpEncodingParameters* mediumEncoding{ nullptr };
		webrtc::RtpEncodingParameters* highEncoding{ nullptr };

		if (!parameters.encodings.empty())
		{
			hasLowEncoding = true;
			//引用赋值，等价于创建别称
			lowEncoding = &parameters.encodings[0];
		}

		if (parameters.encodings.size() > 1)
		{
			hasMediumEncoding = true;
			mediumEncoding    = &parameters.encodings[1];
		}

		if (parameters.encodings.size() > 2)
		{
			hasHighEncoding = true;
			highEncoding    = &parameters.encodings[2];
		}

		// Edit encodings.
		//单层则只支持最低编码
		if (spatialLayer == 1u)
		{
			hasLowEncoding && (lowEncoding->active = true);
			hasMediumEncoding && (mediumEncoding->active = false);
			hasHighEncoding && (highEncoding->active = false);
		}
		//双层则支持中低编码
		else if (spatialLayer == 2u)
		{
			hasLowEncoding && (lowEncoding->active = true);
			hasMediumEncoding && (mediumEncoding->active = true);
			hasHighEncoding && (highEncoding->active = false);
		}
		//三层则支持高中低编码
		else if (spatialLayer == 3u)
		{
			hasLowEncoding && (lowEncoding->active = true);
			hasMediumEncoding && (mediumEncoding->active = true);
			hasHighEncoding && (highEncoding->active = true);
		}

		//更新sender中的RTP参数
		auto result = transceiver->sender()->SetParameters(parameters);

		if (!result.ok())
			MSC_THROW_ERROR("%s", result.message());
	}

	json SendHandler::GetSenderStats(const std::string& localId)
	{
		MSC_TRACE();

		MSC_DEBUG("[localId:%s]", localId.c_str());

		auto localIdIt = this->mapMidTransceiver.find(localId);

		if (localIdIt == this->mapMidTransceiver.end())
			MSC_THROW_ERROR("associated RtpTransceiver not found");

		auto* transceiver = localIdIt->second;
		auto stats        = this->pc->GetStats(transceiver->sender());

		return stats;
	}

	void SendHandler::RestartIce(const json& iceParameters)
	{
		MSC_TRACE();

		// Provide the remote SDP handler with new remote ICE parameters.
		this->remoteSdp->UpdateIceParameters(iceParameters);

		if (!this->transportReady)
			return;

		webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
		options.ice_restart = true;

		// May throw.
		auto offer = this->pc->CreateOffer(options);

		MSC_DEBUG("calling pc->SetLocalDescription():\n%s", offer.c_str());

		// May throw.
		this->pc->SetLocalDescription(PeerConnection::SdpType::OFFER, offer);

		auto localSdpObj = sdptransform::parse(this->pc->GetLocalDescription());
		auto answer      = this->remoteSdp->GetSdp();

		MSC_DEBUG("calling pc->SetRemoteDescription():\n%s", answer.c_str());

		// May throw.
		this->pc->SetRemoteDescription(PeerConnection::SdpType::ANSWER, answer);
	}

	/* RecvHandler methods */

	RecvHandler::RecvHandler(
	  Handler::PrivateListener* privateListener,
	  const json& iceParameters,
	  const json& iceCandidates,
	  const json& dtlsParameters,
	  const json& sctpParameters,
	  const PeerConnection::Options* peerConnectionOptions)
	  : Handler(
	      privateListener, iceParameters, iceCandidates, dtlsParameters, sctpParameters, peerConnectionOptions)
	{
		MSC_TRACE();
	};

	RecvHandler::RecvResult RecvHandler::Receive(
	  const std::string& id, const std::string& kind, const json* rtpParameters)
	{
		MSC_TRACE();

		MSC_DEBUG("[id:%s, kind:%s]", id.c_str(), kind.c_str());

		std::string localId;

		// mid is optional, check whether it exists and is a non empty string.
		auto midIt = rtpParameters->find("mid");
		if (midIt != rtpParameters->end() && (midIt->is_string() && !midIt->get<std::string>().empty()))
			localId = midIt->get<std::string>();
		else
			localId = std::to_string(this->mapMidTransceiver.size());

		const auto& cname = (*rtpParameters)["rtcp"]["cname"];

		//更新远端sdp: offer sdp，此时远端是offer，本地是answer
		this->remoteSdp->CreateOffer(localId, kind, *rtpParameters, cname, id);
		auto offer = this->remoteSdp->GetSdp();

		MSC_DEBUG("calling pc->setRemoteDescription():\n%s", offer.c_str());

		// May throw.
		//设置远端的offer sdp
		this->pc->SetRemoteDescription(PeerConnection::SdpType::OFFER, offer);

		//创建本地answer sdp
		webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
		// May throw.
		auto answer         = this->pc->CreateAnswer(options);
		auto localSdpObject = sdptransform::parse(answer);
		auto mediaIt        = find_if(
      localSdpObject["media"].begin(), localSdpObject["media"].end(), [&localId](const json& m) {
        return m["mid"].get<std::string>() == localId;
      });

		auto& answerMediaObject = *mediaIt;

		// May need to modify codec parameters in the answer based on codec
		// parameters in the offer.
		//基于offer编码参数更新（协商）本地answer的编码参数
		Sdp::Utils::applyCodecParameters(*rtpParameters, answerMediaObject);
		//获得更新后的webrtc sdp
		answer = sdptransform::write(localSdpObject);

		//因为是receiver端，因此属于client，主动发送数据连接请求
		if (!this->transportReady)
			this->SetupTransport("client", localSdpObject);

		MSC_DEBUG("calling pc->SetLocalDescription():\n%s", answer.c_str());

		// May throw.
		//更新本地的sdp: answer sdp
		this->pc->SetLocalDescription(PeerConnection::SdpType::ANSWER, answer);

		//获得对应的Transceiver，不需要主动创建，webrtc内部在响应offer sdp时创建
		auto transceivers  = this->pc->GetTransceivers();
		auto transceiverIt = std::find_if(
		  transceivers.begin(), transceivers.end(), [&localId](webrtc::RtpTransceiverInterface* t) {
			  return t->mid() == localId;
		  });

		if (transceiverIt == transceivers.end())
			MSC_THROW_ERROR("new RTCRtpTransceiver not found");

		auto& transceiver = *transceiverIt;

		// Store in the map.
		this->mapMidTransceiver[localId] = transceiver;

		RecvResult recvResult;

		recvResult.localId     = localId;
		recvResult.rtpReceiver = transceiver->receiver();
		recvResult.track       = transceiver->receiver()->track();

		return recvResult;
	}

	Handler::DataChannel RecvHandler::ReceiveDataChannel(
	  const std::string& label, webrtc::DataChannelInit dataChannelInit)
	{
		MSC_TRACE();

		uint16_t streamId = this->nextSendSctpStreamId;

		dataChannelInit.negotiated = true;
		dataChannelInit.id         = streamId;

		/* clang-format off */
		nlohmann::json sctpStreamParameters =
		{
			{ "streamId", streamId                },
			{ "ordered",  dataChannelInit.ordered }
		};
		/* clang-format on */

		// This will fill sctpStreamParameters's missing fields with default values.
		ortc::validateSctpStreamParameters(sctpStreamParameters);

		rtc::scoped_refptr<webrtc::DataChannelInterface> webrtcDataChannel =
		  this->pc->CreateDataChannel(label, &dataChannelInit);

		// Increase next id.
		this->nextSendSctpStreamId = (this->nextSendSctpStreamId + 1) % SctpNumStreamsMis;

		// If this is the first DataChannel we need to create the SDP answer with
		// m=application section.
		if (!this->hasDataChannelMediaSection)
		{
			this->remoteSdp->RecvSctpAssociation();
			auto sdpOffer = this->remoteSdp->GetSdp();

			MSC_DEBUG("calling pc->setRemoteDescription() [offer:%s]", sdpOffer.c_str());

			// May throw.
			this->pc->SetRemoteDescription(PeerConnection::SdpType::OFFER, sdpOffer);

			webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
			auto sdpAnswer = this->pc->CreateAnswer(options);

			if (!this->transportReady)
			{
				auto localSdpObject = sdptransform::parse(sdpAnswer);
				this->SetupTransport("client", localSdpObject);
			}

			MSC_DEBUG("calling pc->setLocalDescription() [answer: %s]", sdpAnswer.c_str());

			// May throw.
			this->pc->SetLocalDescription(PeerConnection::SdpType::ANSWER, sdpAnswer);

			this->hasDataChannelMediaSection = true;
		}

		RecvHandler::DataChannel dataChannel;

		dataChannel.localId              = std::to_string(streamId);
		dataChannel.dataChannel          = webrtcDataChannel;
		dataChannel.sctpStreamParameters = sctpStreamParameters;

		return dataChannel;
	}

	void RecvHandler::StopReceiving(const std::string& localId)
	{
		MSC_TRACE();

		MSC_DEBUG("[localId:%s]", localId.c_str());

		auto localIdIt = this->mapMidTransceiver.find(localId);

		if (localIdIt == this->mapMidTransceiver.end())
			MSC_THROW_ERROR("associated RtpTransceiver not found");

		auto& transceiver = localIdIt->second;

		MSC_DEBUG("disabling mid:%s", transceiver->mid().value().c_str());

		this->remoteSdp->CloseMediaSection(transceiver->mid().value());

		auto offer = this->remoteSdp->GetSdp();

		MSC_DEBUG("calling pc->setRemoteDescription():\n%s", offer.c_str());

		// May throw.
		this->pc->SetRemoteDescription(PeerConnection::SdpType::OFFER, offer);

		webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;

		// May throw.
		auto answer = this->pc->CreateAnswer(options);

		MSC_DEBUG("calling pc->SetLocalDescription():\n%s", answer.c_str());

		// May throw.
		this->pc->SetLocalDescription(PeerConnection::SdpType::ANSWER, answer);
	}

	json RecvHandler::GetReceiverStats(const std::string& localId)
	{
		MSC_TRACE();

		MSC_DEBUG("[localId:%s]", localId.c_str());

		auto localIdIt = this->mapMidTransceiver.find(localId);

		if (localIdIt == this->mapMidTransceiver.end())
			MSC_THROW_ERROR("associated RtpTransceiver not found");

		auto& transceiver = localIdIt->second;

		// May throw.
		auto stats = this->pc->GetStats(transceiver->receiver());

		return stats;
	}

	void RecvHandler::RestartIce(const json& iceParameters)
	{
		MSC_TRACE();

		// Provide the remote SDP handler with new remote ICE parameters.
		this->remoteSdp->UpdateIceParameters(iceParameters);

		if (!this->transportReady)
			return;

		auto offer = this->remoteSdp->GetSdp();

		MSC_DEBUG("calling pc->setRemoteDescription():\n%s", offer.c_str());

		// May throw.
		this->pc->SetRemoteDescription(PeerConnection::SdpType::OFFER, offer);

		webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;

		// May throw.
		auto answer = this->pc->CreateAnswer(options);

		MSC_DEBUG("calling pc->SetLocalDescription():\n%s", answer.c_str());

		// May throw.
		this->pc->SetLocalDescription(PeerConnection::SdpType::ANSWER, answer);
	}
} // namespace mediasoupclient

// Private helpers used in this file.

static void fillJsonRtpEncodingParameters(json& jsonEncoding, const webrtc::RtpEncodingParameters& encoding)
{
	MSC_TRACE();

	jsonEncoding["active"] = encoding.active;

	if (!encoding.rid.empty())
		jsonEncoding["rid"] = encoding.rid;

	if (encoding.max_bitrate_bps)
		jsonEncoding["maxBitrate"] = *encoding.max_bitrate_bps;

	if (encoding.max_framerate)
		jsonEncoding["maxFramerate"] = *encoding.max_framerate;

	if (encoding.scale_resolution_down_by)
		jsonEncoding["scaleResolutionDownBy"] = *encoding.scale_resolution_down_by;

	jsonEncoding["networkPriority"] = encoding.network_priority;
}
