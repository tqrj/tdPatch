#include "td/telegram/net/RetryAfter.h"
#include "td/utils/tests.h"

TEST(RetryAfter, PreserveScope) {
  for (auto prefix : {td::Slice("FLOOD_WAIT_"), td::Slice("SLOWMODE_WAIT_")}) {
    auto original = td::Status::Error(420, PSLICE() << prefix << 301);
    auto converted = td::retry_after_error(original, 301);
    ASSERT_EQ(converted.code(), 429);
    ASSERT_EQ(converted.message(), original.message());
    ASSERT_EQ(td::retry_after_seconds(converted.code(), converted.message()), 301);
  }
}

TEST(RetryAfter, GenericAndInvalid) {
  auto generic = td::retry_after_error(td::Status::Error(500, "SERVER_ERROR"), 5);
  ASSERT_EQ(generic.message(), "Too Many Requests: retry after 5");
  ASSERT_EQ(td::retry_after_seconds(429, generic.message()), 5);
  ASSERT_EQ(td::retry_after_seconds(420, "FLOOD_WAIT_5"), 0);
  for (auto message : {"FLOOD_WAIT_0", "FLOOD_WAIT_-1", "FLOOD_WAIT_5_extra", "SLOWMODE_WAIT_x"}) {
    ASSERT_EQ(td::retry_after_seconds(429, td::Slice(message)), 0);
  }
  auto unrelated = td::retry_after_error(td::Status::Error(420, "TAKEOUT_INIT_DELAY_5"), 5);
  ASSERT_EQ(unrelated.message(), "Too Many Requests: retry after 5");
}
