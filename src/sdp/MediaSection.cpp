#define MSC_CLASS "Sdp::MediaSection"

#include "sdp/MediaSection.hpp"
#include "Logger.hpp"
#include <algorithm> // ::transform
#include <cctype>    // ::tolower
#include <regex>
#include <sstream>
#include <utility>

using json = nlohmann::json;

// Static functions declaration.
static std::string getCodecName(const json& codec);

namespace mediasoupclient
{
	namespace Sdp
	{
		MediaSection::MediaSection(const json& iceParameters, const json& iceCandidates)
		{
			MSC_TRACE();

			// Set ICE parameters.
			SetIceParameters(iceParameters);

			// Set ICE candidates.
			this->mediaObject["candidates"] = json::array();

			for (auto& candidate : iceCandidates)
			{
				auto candidateObject = json::object();
				//设置a=candidate，详见：https://tools.ietf.org/id/draft-ietf-mmusic-ice-sip-sdp-14.html#rfc.section.5.1
				// mediasoup does mandate rtcp-mux so candidates component is always
				// RTP (1).
				// component = 1 表示Candidate用于RTP，component = 2表示Candidate用于RTCP
				//因为mediasoup采用rtp+rtcp复用一个通道，所以component=1
				candidateObject["component"] = 1;
				// foundation可以理解为candidate的Id，如果两个candidate使用同样的传输协议，比如都是UDP，拥有相同的type，比如都是srflx，host的IP地址相同且端口不同，在STUN和TURN服务器中捕获的IP地址相同，那么e二者的foundation值相同
				//详见：https://tools.ietf.org/html/draft-ietf-ice-rfc5245bis-00#section-4.1.1.3
				candidateObject["foundation"] = candidate["foundation"];
				// candidate的ip地址
				candidateObject["ip"] = candidate["ip"];
				// candidate的端口号
				candidateObject["port"] = candidate["port"];
				// candiate的优先级，介于1-2^32-1之间
				candidateObject["priority"] = candidate["priority"];
				// candidate使用的传输协议，一般的UDP
				candidateObject["transport"] = candidate["protocol"];
				// candidate的类型，包括:
				// host: 远端主机IP，如果是在局域网则是局域网IP
				// srflx: Server Reflexive,
				// 本地candidate收集过程中，发送请求给STUN服务器过程中经过NAT时分配的地址 prflx: Peer
				// Reflexive，远端发送STUN Binding请求经过NAT时分配的地址
				// relay：Relayed，本地发送请求到TURN服务器时，TURN服务器分配的用于中继的地址和端口
				candidateObject["type"] = candidate["type"];

				// candidate使用TCP连接的标识
				if (candidate.find("tcpType") != candidate.end())
					candidateObject["tcptype"] = candidate["tcpType"];

				this->mediaObject["candidates"].push_back(candidateObject);
			}
			// a=end-of-candidates: 用于标识远端的candidates发送结束
			//详见：https://tools.ietf.org/id/draft-ietf-mmusic-trickle-ice-sip-08.html
			this->mediaObject["endOfCandidates"] = "end-of-candidates";
			// a=ice-options:renomination，设置了renomination之后，aggressive nomination将会自动失效
			//详见：https://tools.ietf.org/html/draft-thatcher-ice-renomination-00#section-3
			this->mediaObject["iceOptions"] = "renomination";
		}

		std::string MediaSection::GetMid() const
		{
			MSC_TRACE();

			return this->mediaObject["mid"].get<std::string>();
		}

		bool MediaSection::IsClosed() const
		{
			MSC_TRACE();

			return this->mediaObject["port"] == 0;
		}

		json MediaSection::GetObject() const
		{
			MSC_TRACE();

			return this->mediaObject;
		}

		void MediaSection::SetIceParameters(const json& iceParameters)
		{
			MSC_TRACE();

			this->mediaObject["iceUfrag"] = iceParameters["usernameFragment"];
			this->mediaObject["icePwd"]   = iceParameters["password"];
		}

		void MediaSection::Disable()
		{
			MSC_TRACE();

			this->mediaObject["direction"] = "inactive";

			this->mediaObject.erase("ext");
			this->mediaObject.erase("ssrcs");
			this->mediaObject.erase("ssrcGroups");
			this->mediaObject.erase("simulcast");
			this->mediaObject.erase("rids");
		}

		void MediaSection::Close()
		{
			MSC_TRACE();

			this->mediaObject["direction"] = "inactive";
			this->mediaObject["port"]      = 0;

			this->mediaObject.erase("ext");
			this->mediaObject.erase("ssrcs");
			this->mediaObject.erase("ssrcGroups");
			this->mediaObject.erase("simulcast");
			this->mediaObject.erase("rids");
			this->mediaObject.erase("extmapAllowMixed");
		}

		//创建Answer SDP中media section
		AnswerMediaSection::AnswerMediaSection(
		  const json& iceParameters,
		  const json& iceCandidates,
		  const json& dtlsParameters,
		  const json& sctpParameters,
		  const json& offerMediaObject,
		  json& offerRtpParameters,
		  json& answerRtpParameters,
		  const json* codecOptions)
		  : MediaSection(iceParameters, iceCandidates)
		{
			MSC_TRACE();
			//获取媒体流类型，音频m=audio或者视频m=video
			// m=
			auto type = offerMediaObject["type"].get<std::string>();
			//媒体流id
			this->mediaObject["mid"]  = offerMediaObject["mid"];
			this->mediaObject["type"] = type;
			//同步offer端使用的协议簇
			this->mediaObject["protocol"] = offerMediaObject["protocol"];
			// c=IN IP4 127.0.0.1
			this->mediaObject["connection"] = { { "ip", "127.0.0.1" }, { "version", 4 } };
			this->mediaObject["port"]       = 7;

			// Set DTLS role.
			auto dtlsRole = dtlsParameters["role"].get<std::string>();

			// m=setup
			if (dtlsRole == "client")
				this->mediaObject["setup"] = "active";
			else if (dtlsRole == "server")
				this->mediaObject["setup"] = "passive";
			else if (dtlsRole == "auto")
				this->mediaObject["setup"] = "actpass";

			//音视频媒体流
			if (type == "audio" || type == "video")
			{
				// a=recvonly
				// TODO： mediasoup似乎上行和下行通道各对应一条transeceiver transport，因此answer端都是只接受数据
				this->mediaObject["direction"] = "recvonly";
				// a=rtpmap：用于描述音视频编码格式、采样率等参数
				this->mediaObject["rtp"] = json::array();
				// a=rtcpFb：用于指定rtcp反馈机制，详见：https://tools.ietf.org/html/rfc5124
				this->mediaObject["rtcpFb"] = json::array();
				// a=fmtp：用于指定一些特定的参数
				this->mediaObject["fmtp"] = json::array();

				// SDP格式化：接收端支持的音视频编码格式参数集合
				for (auto& codec : answerRtpParameters["codecs"])
				{
					// clang-format off
					//a=rtpmap:<payload type> <encoding name>/<clock rate> [/<encoding parameters>]
					json rtp =
					{
						//RTP协议中payload类型：用十进制数字表示，比如RTP/AVP payload type=98
						{ "payload", codec["payloadType"] },
						//编码类型，比如视频有VP8\9、H264等
						{ "codec",   getCodecName(codec)  },
						//采样率
						{ "rate",    codec["clockRate"]   }
					};
					// clang-format on
					// TODO: channels中包含信息是什么？
					if (codec.contains("channels"))
					{
						auto channels = codec["channels"].get<uint8_t>();

						if (channels > 1)
							rtp["encoding"] = channels;
					}

					this->mediaObject["rtp"].push_back(rtp);

					//当前编码的参数集
					json codecParameters = codec["parameters"];
					//集成可选编码到接收端和发送端的编码参数集
					if (codecOptions != nullptr && !codecOptions->empty())
					{
						//获得发送端支持的编码集
						auto& offerCodecs = offerRtpParameters["codecs"];
						//根据payload Type匹配发送端同类型（编码格式相同）的编码
						auto codecIt =
						  find_if(offerCodecs.begin(), offerCodecs.end(), [&codec](json& offerCodec) {
							  return offerCodec["payloadType"] == codec["payloadType"];
						  });

						auto& offerCodec = *codecIt;
						//当前媒体类型
						auto mimeType = codec["mimeType"].get<std::string>();
						//转小写
						std::transform(mimeType.begin(), mimeType.end(), mimeType.begin(), ::tolower);

						// opus音频
						if (mimeType == "audio/opus")
						{
							//是否支持双声道
							auto opusStereoIt = codecOptions->find("opusStereo");
							if (opusStereoIt != codecOptions->end())
							{
								auto opusStereo                          = opusStereoIt->get<int>() > 0;
								offerCodec["parameters"]["sprop-stereo"] = opusStereo ? 1 : 0;
								codecParameters["stereo"]                = opusStereo ? 1 : 0;
							}
							//是否封装层FEC格式
							auto opusFecIt = codecOptions->find("opusFec");
							if (opusFecIt != codecOptions->end())
							{
								auto opusFec                             = opusFecIt->get<bool>();
								offerCodec["parameters"]["useinbandfec"] = opusFec ? 1 : 0;
								codecParameters["useinbandfec"]          = opusFec ? 1 : 0;
							}
							//是否使用Dtx机制：如果检测到当前没有明显的通话声音，则停止连续发静音包，改为定期发送（400ms），适用于voip(voice
							// over internet protocol)这种声音不连续的场景，但是不适用于music场景
							auto opusDtxIt = codecOptions->find("opusDtx");
							if (opusDtxIt != codecOptions->end())
							{
								auto opusDtx                       = opusDtxIt->get<bool>();
								offerCodec["parameters"]["usedtx"] = opusDtx ? 1 : 0;
								codecParameters["usedtx"]          = opusDtx ? 1 : 0;
							}
							//音频最大采样率
							auto opusMaxPlaybackRateIt = codecOptions->find("opusMaxPlaybackRate");
							if (opusMaxPlaybackRateIt != codecOptions->end())
							{
								auto opusMaxPlaybackRate           = opusMaxPlaybackRateIt->get<uint32_t>();
								codecParameters["maxplaybackrate"] = opusMaxPlaybackRate;
							}
							//单个音频包的首选时长(the preferred duration of media represented by a
							// packet)，还可以设置maxptime，详见：https://tools.ietf.org/html/draft-ietf-payload-rtp-opus-11
							auto opusPtimeIt = codecOptions->find("opusPtime");
							if (opusPtimeIt != codecOptions->end())
							{
								auto opusPtime           = opusPtimeIt->get<uint32_t>();
								codecParameters["ptime"] = opusPtime;
							}
						}
						//视频格式：vp8\vp9\h264\h265
						else if (mimeType == "video/vp8" || mimeType == "video/vp9" || mimeType == "video/h264" || mimeType == "video/h265")
						{
							//用于answer端告知offser端其编码器的初始码率，这样可以在会话建立初期就发送适当的码率数据，而不是在不清楚远端编码器能力的情况下被动的发生低码率数据
							auto videoGoogleStartBitrateIt = codecOptions->find("videoGoogleStartBitrate");
							if (videoGoogleStartBitrateIt != codecOptions->end())
							{
								auto videoGoogleStartBitrate = videoGoogleStartBitrateIt->get<uint32_t>();
								codecParameters["x-google-start-bitrate"] = videoGoogleStartBitrate;
							}
							// answer端所支持的最低码率
							auto videoGoogleMaxBitrateIt = codecOptions->find("videoGoogleMaxBitrate");
							if (videoGoogleMaxBitrateIt != codecOptions->end())
							{
								auto videoGoogleMaxBitrate              = videoGoogleMaxBitrateIt->get<uint32_t>();
								codecParameters["x-google-max-bitrate"] = videoGoogleMaxBitrate;
							}
							// answer端所支持的最高码率，用于限制后期计算得出的目标码率
							auto videoGoogleMinBitrateIt = codecOptions->find("videoGoogleMinBitrate");
							if (videoGoogleMinBitrateIt != codecOptions->end())
							{
								auto videoGoogleMinBitrate              = videoGoogleMinBitrateIt->get<uint32_t>();
								codecParameters["x-google-min-bitrate"] = videoGoogleMinBitrate;
							}
						}
					} // end of optional codecs

					// clang-format off
					//a=fmtp:<format> <format specific parameters>
					json fmtp =
					{
						{ "payload", codec["payloadType"] }
					};
					// clang-format on

					std::ostringstream config;
					// codec参数格式化
					for (auto& item : codecParameters.items())
					{
						if (!config.str().empty())
							config << ";";

						config << item.key();
						config << "=";
						if (item.value().is_string())
							config << item.value().get<std::string>();
						else if (item.value().is_number_float())
							config << item.value().get<float>();
						else if (item.value().is_number())
							config << item.value().get<int64_t>();
					}

					// a=fmtp: <payloadtype> <codec parameters>
					if (!config.str().empty())
					{
						fmtp["config"] = config.str();
						this->mediaObject["fmtp"].push_back(fmtp);
					}
					// RTCP反馈机制集合
					// a=rtcp-fb: <payload type> <type> <subtype>
					for (auto& fb : codec["rtcpFeedback"])
					{
						// clang-format off
						this->mediaObject["rtcpFb"].push_back(
							{
								{ "payload", codec["payloadType"] },
								{ "type",    fb["type"]           },
								{ "subtype", fb["parameter"]      }
							});
						// clang-format on
					}
				} // end of for answerRtpParameters["codecs"]

				std::string payloads;
				//汇总offer端支持的所有payload类型
				for (auto& codec : answerRtpParameters["codecs"])
				{
					auto payloadType = codec["payloadType"].get<uint8_t>();

					if (!payloads.empty())
						payloads.append(" ");

					payloads.append(std::to_string(payloadType));
				}

				this->mediaObject["payloads"] = payloads;
				// Header extensions
				this->mediaObject["ext"] = json::array();

				// Don't add a header extension if not present in the offer.
				// header
				// extensions中的属性值必须同时存在于offer端和answer端，因为某个协议或者算法的实现需要两端的配合（个人猜测）
				for (auto& ext : answerRtpParameters["headerExtensions"])
				{
					const auto& localExts = offerMediaObject["ext"];
					auto localExtIt = find_if(localExts.begin(), localExts.end(), [&ext](const json& localExt) {
						return localExt["uri"] == ext["uri"];
					});

					if (localExtIt == localExts.end())
						continue;

					// clang-format off
					this->mediaObject["ext"].push_back(
						{
							{ "uri",   ext["uri"] },
							{ "value", ext["id"]  }
						});
					// clang-format on
				}

				// Allow both 1 byte and 2 bytes length header extensions.
				//允许单字节和双字节长度的header
				// extension字段格式，详见：https://tools.ietf.org/html/rfc5285#section-4.1
				auto extmapAllowMixedIt = offerMediaObject.find("extmapAllowMixed");

				// clang-format off
				if (
					extmapAllowMixedIt != offerMediaObject.end() &&
					extmapAllowMixedIt->is_string()
				)
				// clang-format on
				{
					this->mediaObject["extmapAllowMixed"] = "extmap-allow-mixed";
				}

				// Simulcast.
				//如果offer端支持simulcast，则同步设置offer端的simulcast
				// a=simulcast，详见：https://tools.ietf.org/html/draft-ietf-mmusic-sdp-simulcast-14
				auto simulcastId = offerMediaObject.find("simulcast");
				// rids: RTP Ids，一条simulcast stream对应以一个RTP Id
				auto ridsIt = offerMediaObject.find("rids");

				// clang-format off
				if (
					simulcastId != offerMediaObject.end() &&
					simulcastId->is_object() &&
					ridsIt->is_array()
				)
				// clang-format off
				{
					//因为是mediasoup的answer端都是recvonly，所以只设置recv
					this->mediaObject["simulcast"] = json::object();
					this->mediaObject["simulcast"]["dir1"] = "recv";
					//simulcast流对应的rtp id
					this->mediaObject["simulcast"]["list1"] = (*simulcastId)["list1"];

					this->mediaObject["rids"] = json::array();

					for (const auto& rid : *ridsIt)
					{
						if (rid["direction"] != "send")
							continue;

						// clang-format off
						this->mediaObject["rids"].push_back(
							{
								{ "id", rid["id"] },
								{ "direction", "recv" }
							});
						// clang-format on
					}
				}

				this->mediaObject["rtcpMux"] = "rtcp-mux";
				// a=rtcp-rsize：Reduced-Size RTCP，详见：https://tools.ietf.org/html/rfc5506#section-5
				this->mediaObject["rtcpRsize"] = "rtcp-rsize";
			}
			//媒体流类型是application
			else if (type == "application")
			{
				// RTP payload类型：数据通道
				this->mediaObject["payloads"] = "webrtc-datachannel";
				//采样sctp协议
				this->mediaObject["sctpPort"] = sctpParameters["port"];
				//最大的信息数据字节数
				this->mediaObject["maxMessageSize"] = sctpParameters["maxMessageSize"];
			}
		}

		// a=setup: 这一个属性用于指明哪一端应该负责建立TCP连接
		//详见：https://tools.ietf.org/html/rfc4145#section-4
		void AnswerMediaSection::SetDtlsRole(const std::string& role)
		{
			MSC_TRACE();
			//客户端负责主动建立TCP连接
			if (role == "client")
				this->mediaObject["setup"] = "active";
			//服务端负责被动接受TCP连接
			else if (role == "server")
				this->mediaObject["setup"] = "passive";
			//既可以是client，也可以是server
			else if (role == "auto")
				this->mediaObject["setup"] = "actpass";
		}

		//创建offer sdp中的media secion
		OfferMediaSection::OfferMediaSection(
		  const json& iceParameters,
		  const json& iceCandidates,
		  const json& /*dtlsParameters*/, //
		  const json& sctpParameters,
		  const std::string& mid,
		  const std::string& kind,
		  const json& offerRtpParameters,
		  const std::string& streamId,
		  const std::string& trackId)
		  : MediaSection(iceParameters, iceCandidates)
		{
			MSC_TRACE();

			this->mediaObject["mid"]  = mid;
			this->mediaObject["type"] = kind;

			if (sctpParameters == nullptr)
				this->mediaObject["protocol"] = "UDP/TLS/RTP/SAVPF";
			else
				this->mediaObject["protocol"] = "UDP/DTLS/SCTP";

			this->mediaObject["connection"] = { { "ip", "127.0.0.1" }, { "version", 4 } };
			this->mediaObject["port"]       = 7;

			// Set DTLS role.
			// a=setup:
			// actpass，表示Offer端即可以是连接发起端，也可以是接收端，根据Answer端的role做自适应，与其相反即可。
			this->mediaObject["setup"] = "actpass";

			if (kind == "audio" || kind == "video")
			{
				this->mediaObject["direction"] = "sendonly";
				this->mediaObject["rtp"]       = json::array();
				this->mediaObject["rtcpFb"]    = json::array();
				this->mediaObject["fmtp"]      = json::array();

				for (const auto& codec : offerRtpParameters["codecs"])
				{
					// clang-format off
					json rtp =
					{
						{ "payload", codec["payloadType"] },
						{ "codec",   getCodecName(codec)  },
						{ "rate",    codec["clockRate"]   }
					};
					// clang-format on

					if (codec.contains("channels"))
					{
						auto channels = codec["channels"].get<uint8_t>();

						if (channels > 1)
							rtp["encoding"] = channels;
					}

					this->mediaObject["rtp"].push_back(rtp);

					const json& codecParameters = codec["parameters"];

					// clang-format off
					json fmtp =
					{
						{ "payload", codec["payloadType"] }
					};
					// clang-format on

					std::ostringstream config;

					for (auto& item : codecParameters.items())
					{
						if (!config.str().empty())
							config << ";";

						config << item.key();
						config << "=";
						if (item.value().is_string())
							config << item.value().get<std::string>();
						else if (item.value().is_number_float())
							config << item.value().get<float>();
						else if (item.value().is_number())
							config << item.value().get<int64_t>();
					}

					if (!config.str().empty())
					{
						fmtp["config"] = config.str();
						this->mediaObject["fmtp"].push_back(fmtp);
					}

					for (const auto& fb : codec["rtcpFeedback"])
					{
						// clang-format off
						this->mediaObject["rtcpFb"].push_back(
							{
								{ "payload", codec["payloadType"] },
								{ "type",    fb["type"]           },
								{ "subtype", fb["parameter"]      }
							});
						// clang-format on
					}
				}

				std::string payloads;

				for (const auto& codec : offerRtpParameters["codecs"])
				{
					auto payloadType = codec["payloadType"].get<uint8_t>();

					if (!payloads.empty())
						payloads.append(" ");

					payloads.append(std::to_string(payloadType));
				}

				this->mediaObject["payloads"] = payloads;
				this->mediaObject["ext"]      = json::array();

				for (const auto& ext : offerRtpParameters["headerExtensions"])
				{
					// clang-format off
					this->mediaObject["ext"].push_back(
						{
							{ "uri",   ext["uri"] },
							{ "value", ext["id"]  }
						});
					// clang-format on
				}

				this->mediaObject["rtcpMux"]   = "rtcp-mux";
				this->mediaObject["rtcpRsize"] = "rtcp-rsize";

				const auto& encoding = offerRtpParameters["encodings"][0];
				auto ssrc            = encoding["ssrc"].get<uint32_t>();
				uint32_t rtxSsrc;

				auto rtxIt = encoding.find("rtx");
				if ((rtxIt != encoding.end()) && ((*rtxIt).find("ssrc") != (*rtxIt).end()))
					rtxSsrc = encoding["rtx"]["ssrc"].get<uint32_t>();
				else
					rtxSsrc = 0u;

				this->mediaObject["ssrcs"]      = json::array();
				this->mediaObject["ssrcGroups"] = json::array();

				auto cnameIt = offerRtpParameters["rtcp"].find("cname");
				if (cnameIt != offerRtpParameters["rtcp"].end() && cnameIt->is_string())
				{
					auto cname = (*cnameIt).get<std::string>();

					std::string msid(streamId);
					msid.append(" ").append(trackId);

					this->mediaObject["ssrcs"].push_back(
					  { { "id", ssrc }, { "attribute", "cname" }, { "value", cname } });

					this->mediaObject["ssrcs"].push_back(
					  { { "id", ssrc }, { "attribute", "msid" }, { "value", msid } });

					if (rtxSsrc != 0u)
					{
						std::string ssrcs = std::to_string(ssrc).append(" ").append(std::to_string(rtxSsrc));

						this->mediaObject["ssrcs"].push_back(
						  { { "id", rtxSsrc }, { "attribute", "cname" }, { "value", cname } });

						this->mediaObject["ssrcs"].push_back(
						  { { "id", rtxSsrc }, { "attribute", "msid" }, { "value", msid } });

						// Associate original and retransmission SSRCs.
						this->mediaObject["ssrcGroups"].push_back({ { "semantics", "FID" }, { "ssrcs", ssrcs } });
					}
				}
			}
			else if (kind == "application")
			{
				this->mediaObject["payloads"]       = "webrtc-datachannel";
				this->mediaObject["sctpPort"]       = sctpParameters["port"];
				this->mediaObject["maxMessageSize"] = sctpParameters["maxMessageSize"];
			}
		}

		void OfferMediaSection::SetDtlsRole(const std::string& /* role */)
		{
			MSC_TRACE();

			// The SDP offer must always have a=setup:actpass.
			this->mediaObject["setup"] = "actpass";
		}
	} // namespace Sdp
} // namespace mediasoupclient

// Private helpers used in this file.

static std::string getCodecName(const json& codec)
{
	static const std::regex MimeTypeRegex(
	  "^(audio|video)/", std::regex_constants::ECMAScript | std::regex_constants::icase);

	auto mimeType = codec["mimeType"].get<std::string>();

	return std::regex_replace(mimeType, MimeTypeRegex, "");
}
