#include "td/mtproto/AuthData.h"
#include "td/mtproto/SessionConnection.h"
#include "td/mtproto/Transport.h"
#include "td/telegram/Global.h"
#include "td/telegram/MessageContentType.h"
#include "td/telegram/MessageQueryManager.h"
#include "td/telegram/net/RetryAfter.h"
#include "td/utils/Storer.h"
#include "td/utils/Time.h"
#include "td/utils/tests.h"
#include "td/utils/tl_parsers.h"
#include "td/utils/tl_storers.h"

#include <set>

TEST(Patch, SendErrorKeepsWaitScope) {
  for (auto prefix : {td::Slice("FLOOD_WAIT_"), td::Slice("SLOWMODE_WAIT_")}) {
    auto delayed = td::retry_after_error(td::Status::Error(420, PSLICE() << prefix << 301), 17);
    auto result = td::MessageQueryManager::process_send_message_fail_error(
        std::move(delayed), td::DialogId(), false, false, td::MessageContentType::Text);
    ASSERT_EQ(429, result.code());
    ASSERT_EQ(PSTRING() << prefix << 17, result.message());
    ASSERT_EQ(17, td::Global::get_retry_after(result));
  }
}

TEST(Patch, DesktopPadding) {
  td::mtproto::AuthKey key(1, td::string(256, 'a'));
  // MTProto header is 40 bytes before the supplied message; Telegram Desktop pads TL-aligned
  // messages to 12..264 bytes. Every 16-byte bucket must remain reachable.
  for (auto size : {16, 20, 24, 28, 1024}) {
    std::set<size_t> paddings;
    td::string payload(size, 'a');
    for (int i = 0; i < 4096; i++) {
      td::mtproto::PacketInfo info;
      info.version = 2;
      info.use_random_padding = true;
      info.session_id = 1;
      auto packet = td::mtproto::Transport::write(td::SliceStorer(payload), key, &info);
      auto padding = packet.as_slice().size() - 40 - payload.size();
      ASSERT_TRUE(padding >= 12 && padding <= 264);
      ASSERT_EQ(0u, (packet.as_slice().size() - 24) % 16);
      paddings.insert(padding);
    }
    ASSERT_EQ(16u, paddings.size());
  }
}

namespace {
using td::mtproto::MessageId;

// A transport-boundary fake: records serialized packets and delivers a real TL pong.
// No private SessionConnection state is accessed and no socket is opened.
class LoopbackConnection final : public td::mtproto::RawConnection {
 public:
  int ping_count = 0;
  int disconnect_delay = 0;
  bool reply = true;

  void set_connection_token(td::mtproto::ConnectionManager::ConnectionToken) final {
  }
  bool can_send() const final {
    return true;
  }
  td::mtproto::TransportType get_transport_type() const final {
    return {};
  }
  size_t send_crypto(const td::Storer &storer, td::uint64 session_id, td::int64,
                     const td::mtproto::AuthKey &, td::uint64) final {
    session_id_ = session_id;
    td::BufferSlice bytes(storer.size());
    ASSERT_EQ(bytes.size(), storer.store(bytes.as_mutable_slice().ubegin()));
    td::TlParser parser(bytes.as_slice());
    read_message(parser);
    parser.fetch_end();
    CHECK(parser.get_error() == nullptr);
    return bytes.size();
  }
  void send_no_crypto(const td::Storer &) final {
    UNREACHABLE();
  }
  td::PollableFdInfo &get_poll_info() final {
    UNREACHABLE();
  }
  StatsCallback *stats_callback() final {
    return nullptr;
  }
  td::Status flush(const td::mtproto::AuthKey &, Callback &callback) final {
    TRY_STATUS(callback.before_write());
    if (pending_ping_ != 0 && reply) {
      td::mtproto::PacketInfo info;
      info.session_id = session_id_;
      info.message_id = MessageId((static_cast<td::uint64>(td::Time::now() * 4294967296.0) & ~3ULL) | 1);
      td::BufferSlice packet(36);
      td::TlStorerUnsafe storer(packet.as_mutable_slice().ubegin());
      storer.store_long(info.message_id.get());
      storer.store_int(0);
      storer.store_int(20);
      storer.store_int(0x347773c5);
      storer.store_long(pending_message_);
      storer.store_long(pending_ping_);
      pending_ping_ = 0;
      callback.on_read(packet.size());
      TRY_STATUS(callback.on_raw_packet(info, std::move(packet)));
    }
    return td::Status::OK();
  }
  bool has_error() const final {
    return false;
  }
  void close() final {
  }
  PublicFields &extra() final {
    return extra_;
  }
  const PublicFields &extra() const final {
    return extra_;
  }

 private:
  PublicFields extra_;
  td::uint64 session_id_ = 0;
  td::int64 pending_ping_ = 0;
  td::int64 pending_message_ = 0;

  void read_message(td::TlParser &parser) {
    auto message_id = parser.fetch_long();
    parser.fetch_int();
    auto size = parser.fetch_int();
    td::TlParser body(parser.fetch_string_raw<td::Slice>(size));
    auto constructor = body.fetch_int();
    if (constructor == static_cast<td::int32>(0x73f1f8dc)) {
      auto count = body.fetch_int();
      for (int i = 0; i < count; i++) {
        read_message(body);
      }
    } else if (constructor == static_cast<td::int32>(0xf3427b8c)) {
      ping_count++;
      pending_message_ = message_id;
      pending_ping_ = body.fetch_long();
      disconnect_delay = body.fetch_int();
    } else {
      UNREACHABLE();
    }
    body.fetch_end();
    CHECK(body.get_error() == nullptr);
  }
};

class SessionObserver final : public td::mtproto::SessionConnection::Callback {
 public:
  bool closed = false;
  int pongs = 0;
  td::string error;

  void on_connected() final {
  }
  void on_closed(td::Status status) final {
    closed = true;
    if (status.is_error()) {
      error = status.message().str();
    }
  }
  void on_server_salt_updated() final {
  }
  void on_server_time_difference_updated(bool) final {
  }
  void on_new_session_created(td::uint64, MessageId) final {
  }
  void on_session_failed(td::Status) final {
    UNREACHABLE();
  }
  void on_container_sent(MessageId, td::vector<MessageId>) final {
  }
  td::Status on_pong(double, double, double) final {
    pongs++;
    return td::Status::OK();
  }
  td::Status on_update(td::BufferSlice) final {
    UNREACHABLE();
  }
  void on_message_ack(MessageId) final {
  }
  td::Status on_message_result_ok(MessageId, td::BufferSlice, size_t) final {
    UNREACHABLE();
  }
  void on_message_result_error(MessageId, int, td::string) final {
    UNREACHABLE();
  }
  void on_message_failed(MessageId, td::Status) final {
    UNREACHABLE();
  }
  void on_message_info(MessageId, td::int32, MessageId, td::int32, td::int32) final {
    UNREACHABLE();
  }
  td::Status on_destroy_auth_key() final {
    UNREACHABLE();
  }
};

void check_ping(bool is_main, bool reply) {
  td::mtproto::AuthData auth;
  auth.set_use_pfs(false);
  auth.set_main_auth_key(td::mtproto::AuthKey(1, td::string(256, 'a')));
  auth.set_session_id(1);
  auto start = td::Time::now();
  auth.reset_server_time_difference(0);
  auth.set_future_salts({{123, start - 1, start + 10000}, {456, start + 10000, start + 20000}}, start);
  auto connection = td::make_unique<LoopbackConnection>();
  auto *wire = connection.get();
  wire->reply = reply;
  td::mtproto::SessionConnection session(td::mtproto::SessionConnection::Mode::Tcp, std::move(connection), &auth);
  session.set_online(true, is_main);
  SessionObserver observer;
  session.flush(&observer);
  ASSERT_EQ(1, wire->ping_count);
  ASSERT_EQ(60, wire->disconnect_delay);
  ASSERT_EQ(reply ? 1 : 0, observer.pongs);
  ASSERT_FALSE(observer.closed);

  if (!reply) {
    td::Time::jump_in_future(start + 61);
    session.flush(&observer);
    ASSERT_TRUE(observer.closed);
    ASSERT_TRUE(observer.error.find("Ping timeout") != td::string::npos);
    return;
  }
  td::Time::jump_in_future(start + 29);
  auto wakeup = session.flush(&observer);
  ASSERT_EQ(1, wire->ping_count);
  ASSERT_TRUE(wakeup > td::Time::now());
  td::Time::jump_in_future(start + 46);
  wakeup = session.flush(&observer);
  ASSERT_EQ(is_main ? 2 : 1, wire->ping_count);
  ASSERT_FALSE(observer.closed);
  // Recompute after the pong: must never retain an already elapsed ping/pong deadline.
  td::Time::jump_in_future(start + 70);
  wakeup = session.flush(&observer);
  ASSERT_TRUE(wakeup > td::Time::now());
  ASSERT_FALSE(observer.closed);
  if (!is_main) {
    td::Time::jump_in_future(start + 299);
    wakeup = session.flush(&observer);
    ASSERT_EQ(1, wire->ping_count);
    ASSERT_FALSE(observer.closed);
    ASSERT_TRUE(wakeup > td::Time::now());
    td::Time::jump_in_future(start + 301);
    session.flush(&observer);
    ASSERT_TRUE(observer.closed);
    ASSERT_TRUE(observer.error.find("Read timeout") != td::string::npos);
  } else {
    session.force_close(&observer);
  }
}
}  // namespace

TEST(Patch, MainSessionPing) {
  check_ping(true, true);
}
TEST(Patch, MediaSessionIdle) {
  check_ping(false, true);
}
TEST(Patch, MissingPongClosesMain) {
  check_ping(true, false);
}
TEST(Patch, MissingPongClosesMedia) {
  check_ping(false, false);
}
