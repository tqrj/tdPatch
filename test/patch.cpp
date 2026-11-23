#include "td/telegram/net/MtprotoHeader.h"
#include "td/utils/tests.h"
#include "td/utils/tl_parsers.h"

TEST(Patch, Layer234Header) {
  td::MtprotoHeader::Options options;
  options.api_id = 12345;
  options.device_model = "Offline test device";
  options.system_version = "Test OS";
  options.application_version = "7.4.4";
  options.system_language_code = "en";
  td::MtprotoHeader header(options);
  for (const auto &bytes : {header.get_default_header(), header.get_anonymous_header()}) {
    td::TlParser parser(bytes);
    ASSERT_EQ(static_cast<td::int32>(0xda9b0d0d), parser.fetch_int());
    ASSERT_EQ(234, parser.fetch_int());
    ASSERT_EQ(static_cast<td::int32>(0xc1cd5ea9), parser.fetch_int());
    ASSERT_TRUE(parser.get_error() == nullptr);
  }
}
