#include "td/actor/ConcurrentScheduler.h"
#include "td/actor/actor.h"
#include "td/telegram/Td.h"
#include "td/telegram/TdCallback.h"
#include "td/utils/port/path.h"
#include "td/utils/port/thread_local.h"
#include "td/utils/tests.h"

namespace {
class UpdateDeliveryProbe final : public td::Actor {
 public:
  explicit UpdateDeliveryProbe(bool *closed) : closed_(closed) {
  }

  void on_result(td::uint64 id, td::td_api::object_ptr<td::td_api::Object> result) {
    if (id == 1) {
      ASSERT_EQ(td::td_api::authorizationStateWaitTdlibParameters::ID, result->get_id());
      collecting_ = true;
      send_updates();
    } else if (id == 2) {
      // The request is queued after every injected update, so this is a delivery barrier, not a timed sleep.
      ASSERT_EQ(td::td_api::authorizationStateWaitTdlibParameters::ID, result->get_id());
      ASSERT_EQ(expected_, received_);
      collecting_ = false;
      td::send_closure(td_, &td::Td::request, 3, td::td_api::make_object<td::td_api::close>());
    } else if (id == 3) {
      ASSERT_EQ(td::td_api::ok::ID, result->get_id());
    } else if (id == 0) {
      if (collecting_) {
        received_.push_back(result->get_id());
      }
      if (result->get_id() == td::td_api::updateAuthorizationState::ID &&
          static_cast<td::td_api::updateAuthorizationState *>(result.get())->authorization_state_->get_id() ==
              td::td_api::authorizationStateClosed::ID) {
        *closed_ = true;
        td_.reset();
        td::Scheduler::instance()->finish();
        stop();
      }
    } else {
      UNREACHABLE();
    }
  }

 private:
  class Callback final : public td::TdCallback {
   public:
    explicit Callback(td::ActorId<UpdateDeliveryProbe> owner) : owner_(owner) {
    }
    void on_result(td::uint64 id, td::td_api::object_ptr<td::td_api::Object> result) final {
      td::send_closure(owner_, &UpdateDeliveryProbe::on_result, id, std::move(result));
    }
    void on_error(td::uint64 id, td::td_api::object_ptr<td::td_api::error> error) final {
      LOG(FATAL) << "Update delivery probe failed: " << id << " " << td::td_api::to_string(error);
    }

   private:
    td::ActorId<UpdateDeliveryProbe> owner_;
  };

  bool *closed_;
  bool collecting_ = false;
  td::ActorOwn<td::Td> td_;
  td::vector<td::int32> expected_;
  td::vector<td::int32> received_;

  void start_up() final {
    // No initialization parameters: no database, credentials or network connection is created.
    td_ = td::create_actor<td::Td>("UpdateDeliveryTd", td::make_unique<Callback>(actor_id(this)), td::Td::Options{});
    td::send_closure(td_, &td::Td::request, 1, td::td_api::make_object<td::td_api::getAuthorizationState>());
    set_timeout_in(10);
  }

  template <class Update>
  void discard() {
    td::send_closure(td_, &td::Td::send_update, td::td_api::make_object<Update>());
  }

  template <class Update, class... Args>
  void keep(Args &&...args) {
    expected_.push_back(Update::ID);
    td::send_closure(td_, &td::Td::send_update, td::td_api::make_object<Update>(std::forward<Args>(args)...));
  }

  void send_updates() {
    // TGDirector's existing updateHub discard list is the contract, not all updates of a feature family.
    discard<td::td_api::updateFile>();
    discard<td::td_api::updateChatAction>();
    discard<td::td_api::updateUserStatus>();
    discard<td::td_api::updateChatOnlineMemberCount>();
    discard<td::td_api::updateStickerSet>();
    discard<td::td_api::updateInstalledStickerSets>();
    discard<td::td_api::updateTrendingStickerSets>();
    discard<td::td_api::updateRecentStickers>();
    discard<td::td_api::updateFavoriteStickers>();
    discard<td::td_api::updateSavedAnimations>();
    discard<td::td_api::updateChatBackground>();
    discard<td::td_api::updateChatTheme>();
    discard<td::td_api::updateDefaultBackground>();
    discard<td::td_api::updateEmojiChatThemes>();
    discard<td::td_api::updateChatAccentColors>();
    discard<td::td_api::updateAccentColors>();
    discard<td::td_api::updateProfileAccentColors>();
    discard<td::td_api::updateStory>();
    discard<td::td_api::updateStoryDeleted>();
    discard<td::td_api::updateChatActiveStories>();
    discard<td::td_api::updateStoryListChatCount>();
    discard<td::td_api::updateStoryStealthMode>();

    keep<td::td_api::updateAuthorizationState>(
        td::td_api::make_object<td::td_api::authorizationStateWaitTdlibParameters>());
    keep<td::td_api::updateConnectionState>(td::td_api::make_object<td::td_api::connectionStateReady>());
    keep<td::td_api::updateFreezeState>();
    keep<td::td_api::updateNewMessage>();
    keep<td::td_api::updateMessageSendAcknowledged>();
    keep<td::td_api::updateMessageSendSucceeded>();
    keep<td::td_api::updateMessageSendFailed>();
    keep<td::td_api::updateStoryPostSucceeded>();
    keep<td::td_api::updateStoryPostFailed>();
    keep<td::td_api::updateChatReadInbox>();
    keep<td::td_api::updateUser>();
    keep<td::td_api::updateOption>("probe", td::td_api::make_object<td::td_api::optionValueBoolean>(true));
    td::send_closure(td_, &td::Td::request, 2, td::td_api::make_object<td::td_api::getAuthorizationState>());
  }

  void timeout_expired() final {
    LOG(FATAL) << "Update delivery probe did not close";
  }
};

class NetworkStatisticsProbe final : public td::Actor {
 public:
  NetworkStatisticsProbe(td::string dir, bool enable_statistics, bool *closed)
      : dir_(std::move(dir)), enable_statistics_(enable_statistics), closed_(closed) {
  }

  void on_result(td::uint64 id, td::td_api::object_ptr<td::td_api::Object> result) {
    if (id == 0) {
      if (result->get_id() == td::td_api::updateAuthorizationState::ID &&
          static_cast<td::td_api::updateAuthorizationState *>(result.get())->authorization_state_->get_id() ==
              td::td_api::authorizationStateClosed::ID) {
        ASSERT_EQ(4, checked_);
        *closed_ = true;
        td_.reset();
      }
      return;
    }
    if (id == 1 || id == 7 || id == 8) {
      ASSERT_EQ(td::td_api::ok::ID, result->get_id());
      return;
    }
    if (id == 3) {
      ASSERT_EQ(td::td_api::ok::ID, result->get_id());
      td::send_closure(td_, &td::Td::request, 4,
                       td::td_api::make_object<td::td_api::getOption>("disable_network_statistics"));
      td::send_closure(td_, &td::Td::request, 5, td::td_api::make_object<td::td_api::getNetworkStatistics>(false));
      td::send_closure(td_, &td::Td::request, 6, td::td_api::make_object<td::td_api::resetNetworkStatistics>());
      return;
    }
    if (id == 4) {
      ASSERT_EQ(td::td_api::optionValueBoolean::ID, result->get_id());
      ASSERT_EQ(!enable_statistics_, static_cast<td::td_api::optionValueBoolean *>(result.get())->value_);
    } else {
      ASSERT_TRUE(enable_statistics_);
      ASSERT_TRUE(id == 2 || id == 5 || id == 6);
      ASSERT_EQ(id == 6 ? td::td_api::ok::ID : td::td_api::networkStatistics::ID, result->get_id());
    }
    checked();
  }

  void on_destroyed() {
    ASSERT_TRUE(*closed_);
    td::Scheduler::instance()->finish();
    stop();
  }

  void on_error(td::uint64 id, td::td_api::object_ptr<td::td_api::error> error) {
    ASSERT_TRUE(!enable_statistics_);
    ASSERT_TRUE(id == 2 || id == 5 || id == 6);
    ASSERT_EQ(400, error->code_);
    ASSERT_EQ("Network statistics are disabled", error->message_);
    checked();
  }

 private:
  class Callback final : public td::TdCallback {
   public:
    explicit Callback(td::ActorId<NetworkStatisticsProbe> owner) : owner_(owner) {
    }
    ~Callback() final {
      td::send_closure_later(owner_, &NetworkStatisticsProbe::on_destroyed);
    }
    void on_result(td::uint64 id, td::td_api::object_ptr<td::td_api::Object> result) final {
      td::send_closure_later(owner_, &NetworkStatisticsProbe::on_result, id, std::move(result));
    }
    void on_error(td::uint64 id, td::td_api::object_ptr<td::td_api::error> error) final {
      td::send_closure_later(owner_, &NetworkStatisticsProbe::on_error, id, std::move(error));
    }

   private:
    td::ActorId<NetworkStatisticsProbe> owner_;
  };

  td::string dir_;
  bool enable_statistics_;
  bool *closed_;
  int checked_ = 0;
  td::ActorOwn<td::Td> td_;

  void checked() {
    if (++checked_ == 4) {
      td::send_closure(td_, &td::Td::request, 7, td::td_api::make_object<td::td_api::close>());
    }
  }

  void start_up() final {
    td_ = td::create_actor<td::Td>("NetworkStatisticsTd", td::make_unique<Callback>(actor_id(this)), td::Td::Options{});
    td::send_closure(td_, &td::Td::request, 1,
                     td::td_api::make_object<td::td_api::setNetworkType>(td::td_api::make_object<td::td_api::networkTypeNone>()));
    if (enable_statistics_) {
      td::send_closure(td_, &td::Td::request, 8,
                       td::td_api::make_object<td::td_api::setOption>(
                           "disable_network_statistics", td::td_api::make_object<td::td_api::optionValueBoolean>(false)));
    }
    // Queue before initialization to catch a preset applied after NetStatsManager has already been created.
    td::send_closure(td_, &td::Td::request, 2, td::td_api::make_object<td::td_api::getNetworkStatistics>(true));
    auto parameters = td::td_api::make_object<td::td_api::setTdlibParameters>();
    parameters->database_directory_ = dir_;
    parameters->files_directory_ = dir_;
    parameters->api_id_ = 12345;
    parameters->api_hash_ = "0123456789abcdef0123456789abcdef";
    parameters->device_model_ = "Offline test device";
    parameters->system_version_ = "Test OS";
    parameters->application_version_ = "7.4.4";
    parameters->system_language_code_ = "en";
    td::send_closure(td_, &td::Td::request, 3, std::move(parameters));
    set_timeout_in(10);
  }

  void timeout_expired() final {
    LOG(FATAL) << "Network statistics probe did not close";
  }
};

void check_network_statistics(const td::string &dir, bool enable_statistics) {
  // Standalone schedulers must not reuse the handshake semaphore's thread-local actor ID.
  td::clear_thread_locals();
  bool closed = false;
  td::ConcurrentScheduler scheduler(0, 0);
  {
    auto guard = scheduler.get_main_guard();
    td::create_actor<NetworkStatisticsProbe>("NetworkStatisticsProbe", dir, enable_statistics, &closed).release();
  }
  scheduler.start();
  while (scheduler.run_main(1)) {
  }
  scheduler.finish();
  td::clear_thread_locals();
  ASSERT_TRUE(closed);
}
}  // namespace

TEST(Patch, TGDirectorUpdateDelivery) {
  bool closed = false;
  td::ConcurrentScheduler scheduler(0, 0);
  {
    auto guard = scheduler.get_main_guard();
    td::create_actor<UpdateDeliveryProbe>("UpdateDeliveryProbe", &closed).release();
  }
  scheduler.start();
  while (scheduler.run_main(1)) {
  }
  scheduler.finish();
  ASSERT_TRUE(closed);
}

TEST(Patch, TGDirectorNetworkStatisticsDisabled) {
  auto dir = td::mkdtemp(td::CSlice(), "tdpatch-stats-").move_as_ok();
  check_network_statistics(dir, false);
  td::rmrf(dir).ensure();
}

TEST(Patch, TGDirectorNetworkStatisticsPreauthOverride) {
  auto dir = td::mkdtemp(td::CSlice(), "tdpatch-stats-").move_as_ok();
  check_network_statistics(dir, true);
  td::rmrf(dir).ensure();
}

TEST(Patch, TGDirectorNetworkStatisticsResetOnReopen) {
  auto dir = td::mkdtemp(td::CSlice(), "tdpatch-stats-").move_as_ok();
  check_network_statistics(dir, true);  // Persist an explicit false, as an older client might have done.
  check_network_statistics(dir, false);  // The TGDirector preset wins over the persisted value on the next launch.
  td::rmrf(dir).ensure();
}
