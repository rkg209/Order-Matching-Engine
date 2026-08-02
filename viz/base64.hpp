#pragma once

// Spec 010 T4 originally hand-rolled base64 encode here. Spec 013 T1 lifted it (plus a decode
// this file never had) into common/base64.hpp, since the admin API needs base64url decode too.
// This stays as a two-line forwarder so nothing that already includes viz/base64.hpp breaks.

#include "common/base64.hpp"

namespace velox::viz {

using velox::common::base64Encode;

}  // namespace velox::viz
