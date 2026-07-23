#pragma once

// FrameEncoder: message struct -> wire bytes (Spec 007 T1). The mirror image of decoder.hpp --
// same explicit little-endian byte-at-a-time stores, no struct punning.

#include <cstddef>

#include "protocol/messages.hpp"

namespace velox::protocol {

// Every encode function writes a complete frame ([length][msgType][payload]) into `dst`, which
// must have at least `kFrameHeaderSize + kMsgTypeSize + kMaxFrame` bytes available, and returns
// the number of bytes written.
std::size_t encodeLogin(const LoginMsg& m, std::byte* dst) noexcept;
std::size_t encodeNewOrder(const NewOrderMsg& m, std::byte* dst) noexcept;
std::size_t encodeCancel(const CancelMsg& m, std::byte* dst) noexcept;
std::size_t encodeCancelReplace(const CancelReplaceMsg& m, std::byte* dst) noexcept;
std::size_t encodeHeartbeat(const HeartbeatMsg& m, std::byte* dst) noexcept;
std::size_t encodeLoginAck(const LoginAckMsg& m, std::byte* dst) noexcept;
std::size_t encodeLoginReject(const LoginRejectMsg& m, std::byte* dst) noexcept;
std::size_t encodeExecReport(const ExecReportMsg& m, std::byte* dst) noexcept;
std::size_t encodeReject(const RejectMsg& m, std::byte* dst) noexcept;

}  // namespace velox::protocol
