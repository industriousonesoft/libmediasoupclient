#define MSC_CLASS "Sdp::RemoteSdp"

#include "sdp/RemoteSdp.hpp"
#include "Logger.hpp"
#include "sdptransform.hpp"

using json = nlohmann::json;

namespace mediasoupclient
{
	/* Sdp::RemoteSdp methods */

	Sdp::RemoteSdp::RemoteSdp(
	  const json& iceParameters,
	  const json& iceCandidates,
	  const json& dtlsParameters,
	  const json& sctpParameters)
	  : iceParameters(iceParameters), iceCandidates(iceCandidates), dtlsParameters(dtlsParameters),
	    sctpParameters(sctpParameters)
	{
		MSC_TRACE();

		// clang-format off
		this->sdpObject =
		{
			//会话版本： v=
			{ "version", 0 },
			//会话发起者: o=
			{ "origin",
				{
					//单播地址（unicast-address）
					{ "address",        "0.0.0.0"                        },
					//地址类型：IP4 或 IP6
					{ "ipVer",          4                                },
					//网络类型：IN = Internet 
					{ "netType",        "IN"                             },
					//会话Id，建议使用NTP协议(Network Time Protocol)时间戳，以确保唯一性
					{ "sessionId",      10000                            },
					//会话版本号，一旦会话发生修改则会自增
					{ "sessionVersion", 0                                },
					//发起端用户名，不能为空，如果用‘-’表示匿名
					{ "username",       "libmediasoupclient"             }
				}
			},
			//会话名：n=，不呢为空，用'-'表示匿名
			{ "name", "-" },
			//会话起始时间和结束时间，用十进制表示从1900至今的秒数，格式是NTP
		  { "timing",
				{
					//起始时间=0表示会话是永久的
					{ "start", 0 },
					//结束时间=0表示会话永远不会结束，但是开始于start time
					{ "stop",  0 }
				}
			},
			//媒体字段
			{ "media", json::array() }
		};
		// clang-format on

		// If ICE parameters are given, add ICE-Lite indicator.
		// ICE模式分为Full ICE和Lite ICE，详见https://tools.ietf.org/html/rfc5245#section-2.7
		// Full ICE：通信双方都要进行连通性检查，完整的走一遍流程
		// Lite ICE: 只需要Full
		// ICE一方进行连通性检查，Lite一方只需要回应responese消息。这种模式对于部署在公网的设备比较常用
		if (this->iceParameters.find("iceLite") != this->iceParameters.end())
			this->sdpObject["icelite"] = "ice-lite";

		// clang-format off
		//a=msid-semantic:
		//https://tools.ietf.org/id/draft-ietf-mmusic-msid-05.html#rfc.section.3
		this->sdpObject["msidSemantic"] =
		{
			//WMS: WebRTC Media Stream缩写，定义media stream的标识符，一个media stream可能包含多个track(video tracks, audio tracks)
			{ "semantic", "WMS" },
			//Media Stream的id(media session id)
			{ "token",    "*"   }
		};
		// clang-format on

		// NOTE: We take the latest fingerprint.
		//使用最新的指纹信息，基于证书生产的哈希值，用于验证证书的有效性（用于DTLS协商）,详见https://tools.ietf.org/html/rfc5763
		auto numFingerprints = this->dtlsParameters["fingerprints"].size();

		//指纹属性字段：a=fingerprint: <hash algorithm> <hash value>
		this->sdpObject["fingerprint"] = {
			//使用的哈希算法，包括sha-225，sha-256，sha-512等
			{ "type", this->dtlsParameters.at("fingerprints")[numFingerprints - 1]["algorithm"] },
			//哈希值
			{ "hash", this->dtlsParameters.at("fingerprints")[numFingerprints - 1]["value"] }
		};

		// clang-format off
		//media组合属性，a=group: <type> <mid> <mid> ...
		//详见：https://tools.ietf.org/html/draft-ietf-mmusic-sdp-bundle-negotiation-54#section-1.2
		this->sdpObject["groups"] =
		{
			{
				//BUNDLE：表示多路复用
				{ "type", "BUNDLE" },
				{ "mids", ""       }
			}
		};
		// clang-format on
	}

	void Sdp::RemoteSdp::UpdateIceParameters(const json& iceParameters)
	{
		MSC_TRACE();

		this->iceParameters = iceParameters;
		//判读远端是否是Lite Ice，否则是Full Ice
		if (iceParameters.find("iceLite") != iceParameters.end())
			sdpObject["icelite"] = "ice-lite";

		//更新各媒体流中ice协商相关的字段
		for (auto idx{ 0u }; idx < this->mediaSections.size(); ++idx)
		{
			auto* mediaSection = this->mediaSections[idx];
			//设置ice-ufrag(username fragment)，ice-pwd(password)
			mediaSection->SetIceParameters(iceParameters);

			// Update SDP media section.
			this->sdpObject["media"][idx] = mediaSection->GetObject();
		}
	}

	void Sdp::RemoteSdp::UpdateDtlsRole(const std::string& role)
	{
		MSC_TRACE();

		this->dtlsParameters["role"] = role;

		if (iceParameters.find("iceLite") != iceParameters.end())
			sdpObject["icelite"] = "ice-lite";

		for (auto idx{ 0u }; idx < this->mediaSections.size(); ++idx)
		{
			auto* mediaSection = this->mediaSections[idx];

			//更新media stream中的角色屬性： a=setup: <role>
			//詳見：https://tools.ietf.org/html/rfc4145#section-4
			mediaSection->SetDtlsRole(role);

			// Update SDP media section.
			this->sdpObject["media"][idx] = mediaSection->GetObject();
		}
	}
	//获取下一个media section id
	Sdp::RemoteSdp::MediaSectionIdx Sdp::RemoteSdp::GetNextMediaSectionIdx()
	{
		MSC_TRACE();

		// If a closed media section is found, return its index.
		//如果存在已关闭的media section则重用
		for (auto idx{ 0u }; idx < this->mediaSections.size(); ++idx)
		{
			auto* mediaSection = this->mediaSections[idx];

			if (mediaSection->IsClosed())
				return { idx, mediaSection->GetMid() };
		}

		// If no closed media section is found, return next one.
		//如果没有已关闭的media section，则新加一个idx = mediaSections.last_idx + 1 = mediaSections.size
		return { this->mediaSections.size() };
	}

	void Sdp::RemoteSdp::CreateAnswer(
	  json& offerMediaObject,
	  const std::string& reuseMid,
	  json& offerRtpParameters,
	  json& answerRtpParameters,
	  const json* codecOptions)
	{
		MSC_TRACE();
		//结合offer端的参数，以及answer的RtpParameters参数，生产对应的offer端的media section
		auto* mediaSection = new AnswerMediaSection(
		  this->iceParameters,
		  this->iceCandidates,
		  this->dtlsParameters,
		  this->sctpParameters,
		  offerMediaObject,
		  offerRtpParameters,
		  answerRtpParameters,
		  codecOptions);

		//如果是重用的mid则直接更新对应mid的media section
		// Closed media section replacement.
		if (!reuseMid.empty())
		{
			this->ReplaceMediaSection(mediaSection, reuseMid);
		}
		//否则新建一个media section
		else
		{
			this->AddMediaSection(mediaSection);
		}
	}

	void Sdp::RemoteSdp::SendSctpAssociation(json& offerMediaObject)
	{
		nlohmann::json emptyJson;
		auto* mediaSection = new AnswerMediaSection(
		  this->iceParameters,
		  this->iceCandidates,
		  this->dtlsParameters,
		  this->sctpParameters,
		  offerMediaObject,
		  emptyJson,
		  emptyJson,
		  nullptr);

		this->AddMediaSection(mediaSection);
	}

	void Sdp::RemoteSdp::RecvSctpAssociation()
	{
		nlohmann::json emptyJson;
		auto* mediaSection = new OfferMediaSection(
		  this->iceParameters,
		  this->iceCandidates,
		  this->dtlsParameters,
		  this->sctpParameters,
		  "datachannel", // mid
		  "application", // kind
		  emptyJson,     // offerRtpParameters
		  "",            // streamId
		  ""             // trackId
		);
		this->AddMediaSection(mediaSection);
	}

	void Sdp::RemoteSdp::CreateOffer(
	  const std::string& mid,
	  const std::string& kind,
	  const json& offerRtpParameters,
	  const std::string& streamId,
	  const std::string& trackId)
	{
		MSC_TRACE();

		auto* mediaSection = new OfferMediaSection(
		  this->iceParameters,
		  this->iceCandidates,
		  this->dtlsParameters,
		  nullptr, // sctpParameters must be null here.
		  mid,
		  kind,
		  offerRtpParameters,
		  streamId,
		  trackId);

		this->AddMediaSection(mediaSection);
	}

	void Sdp::RemoteSdp::DisableMediaSection(const std::string& mid)
	{
		MSC_TRACE();

		const auto idx     = this->midToIndex[mid];
		auto* mediaSection = this->mediaSections[idx];

		mediaSection->Disable();
	}

	void Sdp::RemoteSdp::CloseMediaSection(const std::string& mid)
	{
		MSC_TRACE();

		const auto idx     = this->midToIndex[mid];
		auto* mediaSection = this->mediaSections[idx];

		// NOTE: Closing the first m section is a pain since it invalidates the
		// bundled transport, so let's avoid it.
		if (mid == this->firstMid)
			mediaSection->Disable();
		else
			mediaSection->Close();

		// Update SDP media section.
		this->sdpObject["media"][idx] = mediaSection->GetObject();

		// Regenerate BUNDLE mids.
		this->RegenerateBundleMids();
	}

	std::string Sdp::RemoteSdp::GetSdp()
	{
		MSC_TRACE();

		// Increase SDP version.
		auto version = this->sdpObject["origin"]["sessionVersion"].get<uint32_t>();

		this->sdpObject["origin"]["sessionVersion"] = ++version;

		return sdptransform::write(this->sdpObject);
	}

	void Sdp::RemoteSdp::AddMediaSection(MediaSection* newMediaSection)
	{
		MSC_TRACE();

		if (this->firstMid.empty())
			this->firstMid = newMediaSection->GetMid();

		// Add it in the vector.
		this->mediaSections.push_back(newMediaSection);

		// Add to the map.
		this->midToIndex[newMediaSection->GetMid()] = this->mediaSections.size() - 1;

		// Add to the SDP object.
		this->sdpObject["media"].push_back(newMediaSection->GetObject());

		this->RegenerateBundleMids();
	}

	void Sdp::RemoteSdp::ReplaceMediaSection(MediaSection* newMediaSection, const std::string& reuseMid)
	{
		MSC_TRACE();

		// Store it in the map.
		if (!reuseMid.empty())
		{
			const auto idx              = this->midToIndex[reuseMid];
			auto* const oldMediaSection = this->mediaSections[idx];

			// Replace the index in the vector with the new media section.
			this->mediaSections[idx] = newMediaSection;

			// Update the map.
			this->midToIndex.erase(oldMediaSection->GetMid());
			this->midToIndex[newMediaSection->GetMid()] = idx;

			// Delete old MediaSection.
			delete oldMediaSection;

			// Update the SDP object.
			this->sdpObject["media"][idx] = newMediaSection->GetObject();

			// Regenerate BUNDLE mids.
			this->RegenerateBundleMids();
		}
		else
		{
			const auto idx              = this->midToIndex[newMediaSection->GetMid()];
			auto* const oldMediaSection = this->mediaSections[idx];

			// Replace the index in the vector with the new media section.
			this->mediaSections[idx] = newMediaSection;

			// Delete old MediaSection.
			delete oldMediaSection;

			// Update the SDP object.
			this->sdpObject["media"][this->mediaSections.size() - 1] = newMediaSection->GetObject();
		}
	}

	void Sdp::RemoteSdp::RegenerateBundleMids()
	{
		MSC_TRACE();

		std::string mids;

		for (const auto* mediaSection : this->mediaSections)
		{
			if (!mediaSection->IsClosed())
			{
				if (mids.empty())
					mids = mediaSection->GetMid();
				else
					mids.append(" ").append(mediaSection->GetMid());
			}
		}

		this->sdpObject["groups"][0]["mids"] = mids;
	}
} // namespace mediasoupclient
