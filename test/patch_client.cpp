#include "td/actor/ConcurrentScheduler.h"
#include "td/actor/actor.h"
#include "td/telegram/Global.h"
#include "td/telegram/Td.h"
#include "td/telegram/TdCallback.h"
#include "td/telegram/net/MtprotoHeader.h"
#include "td/telegram/net/NetQueryCreator.h"
#include "td/telegram/telegram_api.h"
#include "td/utils/port/path.h"
#include "td/utils/tests.h"
#include "td/utils/tl_parsers.h"

namespace {
// Probe the real Td parameter handling and query encoder in its normal actor context.
void check_outbound() {
  auto bytes = td::G()->mtproto_header().get_default_header();
  td::TlParser parser(bytes);
  ASSERT_EQ(static_cast<td::int32>(0xda9b0d0d), parser.fetch_int());
  ASSERT_EQ(233, parser.fetch_int());
  ASSERT_EQ(static_cast<td::int32>(0xc1cd5ea9), parser.fetch_int());
  parser.fetch_int();  // flags
  ASSERT_EQ(12345, parser.fetch_int());
  ASSERT_EQ("Offline test device", parser.fetch_string<td::string>());
  ASSERT_EQ("Test OS", parser.fetch_string<td::string>());
  // Must traverse Td::get_parameters: constructing MtprotoHeader directly wouldn't catch the suffix.
  ASSERT_EQ("7.4.4", parser.fetch_string<td::string>());
  ASSERT_EQ("en", parser.fetch_string<td::string>());
  ASSERT_TRUE(parser.get_error() == nullptr);

  td::NetQueryCreator creator(nullptr);
  for (auto size : {64, 256, 16384, 65536}) {
    auto query = creator.create_unauth(
        td::telegram_api::upload_saveFilePart(1, 0, td::BufferSlice(td::string(size, 'a'))));
    ASSERT_TRUE(query->gzip_flag() == td::NetQuery::GzipFlag::Off);
    td::TlParser payload(query->query().as_slice());
    ASSERT_EQ(td::telegram_api::upload_saveFilePart::ID, payload.fetch_int());
    ASSERT_EQ(1, payload.fetch_long());
    ASSERT_EQ(0, payload.fetch_int());
    ASSERT_EQ(td::string(size, 'a'), payload.fetch_string<td::string>());
    payload.fetch_end();
    ASSERT_TRUE(payload.get_error() == nullptr);
    query->set_ok(td::BufferSlice());
  }
}

class ClientProbe final : public td::Actor {
 public:
  ClientProbe(td::string dir, bool *checked) : dir_(std::move(dir)), checked_(checked) {
  }
  void checked() {
    *checked_ = true;
    td::send_closure(td_, &td::Td::request, 3, td::td_api::make_object<td::td_api::close>());
  }
  void closed() {
    td_.reset();
    td::Scheduler::instance()->finish();
    stop();
  }

 private:
  class Callback final : public td::TdCallback {
   public:
    explicit Callback(td::ActorId<ClientProbe> owner) : owner_(owner) {
    }
    void on_result(td::uint64 id, td::td_api::object_ptr<td::td_api::Object> result) final {
      if (id == 2) {
        check_outbound();
        td::send_closure(owner_, &ClientProbe::checked);
      } else if (id == 0 && result->get_id() == td::td_api::updateAuthorizationState::ID &&
                 static_cast<td::td_api::updateAuthorizationState *>(result.get())->authorization_state_->get_id() ==
                     td::td_api::authorizationStateClosed::ID) {
        td::send_closure(owner_, &ClientProbe::closed);
      }
    }
    void on_error(td::uint64 id, td::td_api::object_ptr<td::td_api::error> error) final {
      LOG(FATAL) << "Offline probe failed: " << id << " " << td::td_api::to_string(error);
    }

   private:
    td::ActorId<ClientProbe> owner_;
  };

  td::string dir_;
  bool *checked_;
  td::ActorOwn<td::Td> td_;

  void start_up() final {
    td_ = td::create_actor<td::Td>("PatchTd", td::make_unique<Callback>(actor_id(this)), td::Td::Options{});
    // Disable networking before initialization. No credentials, account or Telegram RPC is used.
    td::send_closure(td_, &td::Td::request, 1,
                     td::td_api::make_object<td::td_api::setNetworkType>(td::td_api::make_object<td::td_api::networkTypeNone>()));
    auto parameters = td::td_api::make_object<td::td_api::setTdlibParameters>();
    parameters->database_directory_ = dir_;
    parameters->files_directory_ = dir_;
    parameters->api_id_ = 12345;
    parameters->api_hash_ = "0123456789abcdef0123456789abcdef";
    parameters->device_model_ = "Offline test device";
    parameters->system_version_ = "Test OS";
    parameters->application_version_ = "7.4.4";
    parameters->system_language_code_ = "en";
    td::send_closure(td_, &td::Td::request, 2, std::move(parameters));
    set_timeout_in(30);
  }
  void timeout_expired() final {
    LOG(FATAL) << "Offline Td probe did not close";
  }
};
}  // namespace

TEST(Patch, ClientFingerprintAndCompression) {
  auto dir = td::mkdtemp(td::CSlice(), "tdpatch-").move_as_ok();
  bool checked = false;
  td::ConcurrentScheduler scheduler(0, 0);
  {
    auto guard = scheduler.get_main_guard();
    td::create_actor<ClientProbe>("PatchClientProbe", dir, &checked).release();
  }
  scheduler.start();
  while (scheduler.run_main(1)) {
  }
  scheduler.finish();
  td::rmrf(dir).ensure();
  ASSERT_TRUE(checked);
}
