#include "WebRTCClientTestFixture.h"

namespace com { namespace amazonaws { namespace kinesis { namespace video { namespace webrtcclient {

class PeerConnectionApiTest : public WebRtcClientTestBase {
};

TEST_F(PeerConnectionApiTest, deserializeRtcIceCandidateInit)
{
    RtcIceCandidateInit rtcIceCandidateInit;

    MEMSET(&rtcIceCandidateInit, 0x00, SIZEOF(rtcIceCandidateInit));

    auto notAnObject = "helloWorld";
    EXPECT_EQ(deserializeRtcIceCandidateInit((PCHAR) notAnObject, STRLEN(notAnObject), &rtcIceCandidateInit), STATUS_INVALID_API_CALL_RETURN_JSON);

    auto emptyObject = "{}";
    EXPECT_EQ(deserializeRtcIceCandidateInit((PCHAR) emptyObject, STRLEN(emptyObject), &rtcIceCandidateInit), STATUS_INVALID_API_CALL_RETURN_JSON);

    auto noCandidate = "{randomKey: \"randomValue\"}";
    EXPECT_EQ(deserializeRtcIceCandidateInit((PCHAR) noCandidate, STRLEN(noCandidate), &rtcIceCandidateInit), STATUS_ICE_CANDIDATE_MISSING_CANDIDATE);

    auto keyNoValue = "{1,2,3,4,5}candidate";
    EXPECT_EQ(deserializeRtcIceCandidateInit((PCHAR) keyNoValue, STRLEN(keyNoValue), &rtcIceCandidateInit), STATUS_ICE_CANDIDATE_MISSING_CANDIDATE);

    auto validCandidate = "{candidate: \"foobar\"}";
    EXPECT_EQ(deserializeRtcIceCandidateInit((PCHAR) validCandidate, STRLEN(validCandidate), &rtcIceCandidateInit), STATUS_SUCCESS);
    EXPECT_STREQ(rtcIceCandidateInit.candidate, "foobar");
}

TEST_F(PeerConnectionApiTest, serializeSessionDescriptionInit)
{
    RtcSessionDescriptionInit rtcSessionDescriptionInit;
    UINT32 sessionDescriptionJSONLen = 0;
    CHAR sessionDescriptionJSON[500] = {0};

    MEMSET(&rtcSessionDescriptionInit, 0x00, SIZEOF(RtcSessionDescriptionInit));

    EXPECT_EQ(serializeSessionDescriptionInit(NULL, sessionDescriptionJSON, &sessionDescriptionJSONLen), STATUS_NULL_ARG);
    EXPECT_EQ(serializeSessionDescriptionInit(&rtcSessionDescriptionInit, sessionDescriptionJSON, NULL), STATUS_NULL_ARG);

    STRCPY(rtcSessionDescriptionInit.sdp, "KVS\nWebRTC\nSDP\nValue\n");
    rtcSessionDescriptionInit.type = SDP_TYPE_OFFER;
    sessionDescriptionJSONLen = 500;

    EXPECT_EQ(serializeSessionDescriptionInit(&rtcSessionDescriptionInit, sessionDescriptionJSON, &sessionDescriptionJSONLen), STATUS_SUCCESS);
    EXPECT_STREQ(sessionDescriptionJSON, "{\"type\": \"offer\", \"sdp\": \"KVS\\r\\nWebRTC\\r\\nSDP\\r\\nValue\\r\\n\"}");
}

TEST_F(PeerConnectionApiTest, suppliedCertificatesVariation)
{
    RtcConfiguration configuration;
    PRtcPeerConnection pRtcPeerConnection;
    RtcCertificate certificates[MAX_RTCCONFIGURATION_CERTIFICATES + 1];
    UINT32 i;

    // Mock some certificates
    for (i = 0; i < ARRAY_SIZE(certificates); i++) {
        certificates->pCertificate = (PBYTE) 1;
        certificates->pPrivateKey = (PBYTE) 2;
        certificates->certificateSize = 100;
        certificates->privateKeySize = 200;
    }

    MEMSET(&configuration, 0x00, SIZEOF(RtcConfiguration));
    configuration.iceTransportPolicy = ICE_TRANSPORT_POLICY_RELAY;

    // Max certs
    configuration.certificates = certificates;
    configuration.certificateCount = MAX_RTCCONFIGURATION_CERTIFICATES + 1;
    EXPECT_EQ(STATUS_SSL_INVALID_CERTIFICATE_COUNT, createPeerConnection(&configuration, &pRtcPeerConnection));

    // No certs
    configuration.certificates = certificates;
    configuration.certificateCount = 0;
    EXPECT_EQ(STATUS_SSL_INVALID_CERTIFICATE_COUNT, createPeerConnection(&configuration, &pRtcPeerConnection));

    // Cert bit is NULL
    certificates[2].pCertificate = NULL;
    configuration.certificates = certificates;
    configuration.certificateCount = MAX_RTCCONFIGURATION_CERTIFICATES;
    EXPECT_EQ(STATUS_SSL_INVALID_CERTIFICATE_BITS, createPeerConnection(&configuration, &pRtcPeerConnection));
    certificates[2].pCertificate = (PBYTE) 3;

    // Certs are null but count is not. Should generate cert OK
    configuration.certificates = NULL;
    configuration.certificateCount = MAX_RTCCONFIGURATION_CERTIFICATES;
    EXPECT_EQ(STATUS_SUCCESS, createPeerConnection(&configuration, &pRtcPeerConnection));
    EXPECT_EQ(STATUS_SUCCESS, freePeerConnection(&pRtcPeerConnection));
}

TEST_F(PeerConnectionApiTest, deserializeSessionDescriptionInit)
{
    RtcSessionDescriptionInit rtcSessionDescriptionInit;
    MEMSET(&rtcSessionDescriptionInit, 0x00, SIZEOF(RtcSessionDescriptionInit));

    auto notAnObject = "helloWorld";
    EXPECT_EQ(deserializeSessionDescriptionInit((PCHAR) notAnObject, STRLEN(notAnObject), &rtcSessionDescriptionInit), STATUS_INVALID_API_CALL_RETURN_JSON);

    auto emptyObject = "{}";
    EXPECT_EQ(deserializeSessionDescriptionInit((PCHAR) emptyObject, STRLEN(emptyObject), &rtcSessionDescriptionInit), STATUS_INVALID_API_CALL_RETURN_JSON);

    auto noSDPKey  = "{type: \"offer\"}";
    EXPECT_EQ(deserializeSessionDescriptionInit((PCHAR) noSDPKey, STRLEN(noSDPKey), &rtcSessionDescriptionInit), STATUS_SESSION_DESCRIPTION_INIT_MISSING_SDP);

    auto noTypeKey  = "{\"sdp\": \"KVS\\r\\nWebRTC\\r\\nSDP\\r\\nValue\\r\\n\"}";
    EXPECT_EQ(deserializeSessionDescriptionInit((PCHAR) noTypeKey, STRLEN(noTypeKey), &rtcSessionDescriptionInit), STATUS_SESSION_DESCRIPTION_INIT_MISSING_TYPE);

    auto invalidTypeKey = "{sdp: \"kvsSdp\", type: \"foobar\"}";
    EXPECT_EQ(deserializeSessionDescriptionInit((PCHAR) invalidTypeKey, STRLEN(invalidTypeKey), &rtcSessionDescriptionInit), STATUS_SESSION_DESCRIPTION_INIT_INVALID_TYPE);

    auto keyNoValue = "{1,2,3,4,5}sdp";
    EXPECT_EQ(deserializeSessionDescriptionInit((PCHAR) keyNoValue, STRLEN(keyNoValue), &rtcSessionDescriptionInit), STATUS_SESSION_DESCRIPTION_INIT_MISSING_SDP);

    auto validSessionDescriptionInit = "{sdp: \"KVS\\r\\nWebRTC\\r\\nSDP\\r\\nValue\\r\\n\", type: \"offer\"}";
    EXPECT_EQ(deserializeSessionDescriptionInit((PCHAR) validSessionDescriptionInit, STRLEN(validSessionDescriptionInit), &rtcSessionDescriptionInit), STATUS_SUCCESS);
    EXPECT_STREQ(rtcSessionDescriptionInit.sdp, "KVS\nWebRTC\nSDP\nValue\n");
    EXPECT_EQ(rtcSessionDescriptionInit.type, SDP_TYPE_OFFER);
}

TEST_F(PeerConnectionApiTest, fmtpForPayloadType)
{
    auto rawSessionDescription = R"(v=0
o=- 686950092 1576880200 IN IP4 0.0.0.0
s=-
t=0 0
m=audio 9 UDP/TLS/RTP/SAVPF 109
a=rtpmap:109 opus/48000/2
a=fmtp:109 minptime=10;useinbandfec=1
m=video 9 UDP/TLS/RTP/SAVPF 97
a=rtpmap:97 H264/90000
a=fmtp:97 profile-level-id=42e01f;level-asymmetry-allowed=1
)";

    SessionDescription sessionDescription;
    MEMSET(&sessionDescription, 0x00, SIZEOF(SessionDescription));
    EXPECT_EQ(serializeSessionDescription(&sessionDescription, (PCHAR) rawSessionDescription), STATUS_SUCCESS);

    EXPECT_STREQ(fmtpForPayloadType(97, &sessionDescription), "profile-level-id=42e01f;level-asymmetry-allowed=1");
    EXPECT_STREQ(fmtpForPayloadType(109, &sessionDescription), "minptime=10;useinbandfec=1");
    EXPECT_STREQ(fmtpForPayloadType(25, &sessionDescription), NULL);

}

}
}
}
}
}
