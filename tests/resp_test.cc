#include <gtest/gtest.h>

#include "commands.h"
#include "resp.h"
#include "test_util.h"

namespace bitkv {
namespace {

using resp::ParseCommand;
using resp::ParseResult;

TEST(Resp, ParsesArrayCommand) {
  std::string in = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";
  std::vector<std::string> args;
  size_t used;
  std::string err;
  ASSERT_EQ(ParseCommand(in, &args, &used, &err), ParseResult::kOk);
  EXPECT_EQ(used, in.size());
  EXPECT_EQ(args, (std::vector<std::string>{"SET", "foo", "bar"}));
}

// TCP delivers bytes in arbitrary chunks. Every strict prefix of a valid
// command must be "incomplete" (never an error, never a wrong parse).
TEST(Resp, EveryPrefixIsIncomplete) {
  std::string in = "*2\r\n$3\r\nGET\r\n$5\r\nhello\r\n";
  for (size_t n = 0; n < in.size(); ++n) {
    std::vector<std::string> args;
    size_t used;
    std::string err;
    EXPECT_EQ(ParseCommand(std::string_view(in).substr(0, n), &args, &used, &err),
              ParseResult::kIncomplete)
        << "prefix length " << n;
  }
}

TEST(Resp, ParsesPipelinedCommands) {
  std::string in = "*1\r\n$4\r\nPING\r\n*2\r\n$3\r\nGET\r\n$1\r\nk\r\n";
  std::vector<std::string> args;
  size_t used;
  std::string err;
  ASSERT_EQ(ParseCommand(in, &args, &used, &err), ParseResult::kOk);
  EXPECT_EQ(args[0], "PING");
  ASSERT_EQ(ParseCommand(std::string_view(in).substr(used), &args, &used, &err),
            ParseResult::kOk);
  EXPECT_EQ(args, (std::vector<std::string>{"GET", "k"}));
}

TEST(Resp, BinarySafeBulkStrings) {
  std::string payload("a\r\nb\0c", 6);
  std::string in = "*1\r\n$6\r\n" + payload + "\r\n";
  std::vector<std::string> args;
  size_t used;
  std::string err;
  ASSERT_EQ(ParseCommand(in, &args, &used, &err), ParseResult::kOk);
  EXPECT_EQ(args[0], payload);
}

TEST(Resp, InlineCommand) {
  std::vector<std::string> args;
  size_t used;
  std::string err;
  ASSERT_EQ(ParseCommand("SET  a   b\r\n", &args, &used, &err), ParseResult::kOk);
  EXPECT_EQ(args, (std::vector<std::string>{"SET", "a", "b"}));
}

TEST(Resp, RejectsMalformedInput) {
  std::vector<std::string> args;
  size_t used;
  std::string err;
  EXPECT_EQ(ParseCommand("*x\r\n", &args, &used, &err), ParseResult::kError);
  EXPECT_EQ(ParseCommand("*1\r\n#3\r\nGET\r\n", &args, &used, &err), ParseResult::kError);
  EXPECT_EQ(ParseCommand("*1\r\n$3\r\nGETXX", &args, &used, &err), ParseResult::kError);
  EXPECT_EQ(ParseCommand("*1\r\n$-5\r\n", &args, &used, &err), ParseResult::kError);
}

class CommandTest : public ::testing::Test {
 protected:
  void SetUp() override { db_ = test::OpenOrDie(dir_.path()); }
  std::string Run(std::vector<std::string> args) {
    std::string out;
    bool quit;
    ExecuteCommand(db_.get(), args, &out, &quit);
    return out;
  }
  test::TempDir dir_;
  std::unique_ptr<DB> db_;
};

TEST_F(CommandTest, SetGetDel) {
  EXPECT_EQ(Run({"SET", "k", "v"}), "+OK\r\n");
  EXPECT_EQ(Run({"get", "k"}), "$1\r\nv\r\n");  // case-insensitive
  EXPECT_EQ(Run({"EXISTS", "k", "nope"}), ":1\r\n");
  EXPECT_EQ(Run({"DEL", "k", "nope"}), ":1\r\n");
  EXPECT_EQ(Run({"GET", "k"}), "$-1\r\n");
}

TEST_F(CommandTest, Incr) {
  EXPECT_EQ(Run({"INCR", "c"}), ":1\r\n");
  EXPECT_EQ(Run({"INCR", "c"}), ":2\r\n");
  EXPECT_EQ(Run({"DECR", "c"}), ":1\r\n");
  Run({"SET", "s", "abc"});
  EXPECT_EQ(Run({"INCR", "s"}).substr(0, 4), "-ERR");
}

TEST_F(CommandTest, ErrorsAndMisc) {
  EXPECT_EQ(Run({"PING"}), "+PONG\r\n");
  EXPECT_EQ(Run({"GET"}).substr(0, 4), "-ERR");
  EXPECT_EQ(Run({"NOSUCH"}).substr(0, 4), "-ERR");
  Run({"SET", "a", "1"});
  EXPECT_EQ(Run({"DBSIZE"}), ":1\r\n");
  EXPECT_EQ(Run({"COMPACT"}), "+OK\r\n");
}

}  // namespace
}  // namespace bitkv
