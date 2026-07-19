#include <unity.h>

// test_build_src = no keeps src/ out of the native build, so the unit under
// test is included directly. serial_proto has no Arduino dependencies.
#include "../../src/serial_proto.cpp"

// Feeds a whole line through the parser and returns the resulting command.
static Command parse(const char *line) {
  SerialProto proto;
  Command last;
  for (const char *p = line; *p; p++) last = proto.feed(*p);
  return proto.feed('\n');
}

void test_select_slot(void) {
  const Command c = parse("S 2");
  TEST_ASSERT_TRUE(c.kind == CommandKind::SelectSlot);
  TEST_ASSERT_EQUAL_UINT8(2, c.slot);
}

void test_pair_slot(void) {
  const Command c = parse("B 0");
  TEST_ASSERT_TRUE(c.kind == CommandKind::PairSlot);
  TEST_ASSERT_EQUAL_UINT8(0, c.slot);
}

void test_forget_slot(void) {
  const Command c = parse("U 3");
  TEST_ASSERT_TRUE(c.kind == CommandKind::ForgetSlot);
  TEST_ASSERT_EQUAL_UINT8(3, c.slot);
}

void test_slot_out_of_range_is_an_error(void) {
  TEST_ASSERT_TRUE(parse("S 4").kind == CommandKind::Error);
  TEST_ASSERT_TRUE(parse("S 9").kind == CommandKind::Error);
}

void test_missing_slot_is_an_error(void) {
  TEST_ASSERT_TRUE(parse("S").kind == CommandKind::Error);
}

void test_trailing_junk_is_an_error(void) {
  TEST_ASSERT_TRUE(parse("S 1 1").kind == CommandKind::Error);
}

void test_multi_digit_slot_is_an_error(void) {
  // Pins the rejection inside parseSlot: multi-digit slot "S 10" must fail
  // at the isspace check after the single digit, not be silently truncated.
  // The atEnd check would otherwise mask this failure.
  TEST_ASSERT_TRUE(parse("S 10").kind == CommandKind::Error);
}

void test_lowercase_verb_works(void) {
  // The existing verbs are case-insensitive; these must not be an exception.
  TEST_ASSERT_TRUE(parse("s 1").kind == CommandKind::SelectSlot);
}

void test_existing_verbs_still_parse(void) {
  const Command k = parse("K 02 04 00 00 00 00 00");
  TEST_ASSERT_TRUE(k.kind == CommandKind::Key);
  TEST_ASSERT_EQUAL_UINT8(0x02, k.key.modifiers);
  TEST_ASSERT_EQUAL_UINT8(0x04, k.key.keys[0]);
  TEST_ASSERT_TRUE(parse("P").kind == CommandKind::Ping);
  TEST_ASSERT_TRUE(parse("R").kind == CommandKind::ReleaseAll);
  TEST_ASSERT_TRUE(parse("C 08 00").kind == CommandKind::Consumer);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_select_slot);
  RUN_TEST(test_pair_slot);
  RUN_TEST(test_forget_slot);
  RUN_TEST(test_slot_out_of_range_is_an_error);
  RUN_TEST(test_missing_slot_is_an_error);
  RUN_TEST(test_trailing_junk_is_an_error);
  RUN_TEST(test_multi_digit_slot_is_an_error);
  RUN_TEST(test_lowercase_verb_works);
  RUN_TEST(test_existing_verbs_still_parse);
  return UNITY_END();
}
