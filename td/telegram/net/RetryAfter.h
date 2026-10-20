#pragma once

#include "td/utils/common.h"
#include "td/utils/misc.h"
#include "td/utils/Slice.h"
#include "td/utils/SliceBuilder.h"
#include "td/utils/Status.h"

namespace td {

// Keep the server's wait scope when a delayed query exceeds its timeout budget.
// Dependent queries without a server error keep the generic retry-after format.
inline Status retry_after_error(const Status &error, int32 timeout) {
  if (error.code() == 420) {
    for (auto prefix : {Slice("FLOOD_WAIT_"), Slice("SLOWMODE_WAIT_")}) {
      if (begins_with(error.message(), prefix)) {
        auto seconds = to_integer_safe<int32>(error.message().substr(prefix.size()));
        if (seconds.is_ok() && seconds.ok() > 0) {
          return Status::Error(429, PSLICE() << prefix << timeout);
        }
      }
    }
  }
  return Status::Error(429, PSLICE() << "Too Many Requests: retry after " << timeout);
}

inline int32 retry_after_seconds(int32 code, Slice message) {
  if (code != 429) {
    return 0;
  }
  for (auto prefix : {Slice("Too Many Requests: retry after "), Slice("FLOOD_WAIT_"), Slice("SLOWMODE_WAIT_")}) {
    if (begins_with(message, prefix)) {
      auto seconds = to_integer_safe<int32>(message.substr(prefix.size()));
      if (seconds.is_ok() && seconds.ok() > 0) {
        return seconds.ok();
      }
    }
  }
  return 0;
}

}  // namespace td
