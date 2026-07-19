// test/test_slot_address/test_slot_address.cpp
#include <unity.h>

#include "../../src/slot_address.h"

// A base MAC with known low bits, so masking is visible in the assertions.
static const uint8_t kBase[6] = {0x24, 0x0A, 0xC4, 0x11, 0x22, 0x33};

void test_top_two_bits_set_for_static_random(void) {
  uint8_t out[6];
  deriveSlotAddress(kBase, 0, out);
  // BLE requires the top two bits of the most significant byte set.
  TEST_ASSERT_EQUAL_HEX8(0xC0, out[0] & 0xC0);
}

void test_slot_index_lands_in_lsb(void) {
  uint8_t out[6];
  deriveSlotAddress(kBase, 2, out);
  TEST_ASSERT_EQUAL_HEX8(0x32, out[5]);  // 0x33 & 0xFC | 2
}

void test_every_slot_differs(void) {
  uint8_t seen[kSlotCount][6];
  for (uint8_t s = 0; s < kSlotCount; s++) deriveSlotAddress(kBase, s, seen[s]);
  for (uint8_t a = 0; a < kSlotCount; a++) {
    for (uint8_t b = a + 1; b < kSlotCount; b++) {
      TEST_ASSERT_NOT_EQUAL(0, memcmp(seen[a], seen[b], 6));
    }
  }
}

void test_derivation_is_stable(void) {
  uint8_t first[6], second[6];
  deriveSlotAddress(kBase, 1, first);
  deriveSlotAddress(kBase, 1, second);
  TEST_ASSERT_EQUAL_MEMORY(first, second, 6);
}

void test_middle_bytes_come_from_the_base_mac(void) {
  uint8_t out[6];
  deriveSlotAddress(kBase, 0, out);
  TEST_ASSERT_EQUAL_MEMORY(kBase + 1, out + 1, 4);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_top_two_bits_set_for_static_random);
  RUN_TEST(test_slot_index_lands_in_lsb);
  RUN_TEST(test_every_slot_differs);
  RUN_TEST(test_derivation_is_stable);
  RUN_TEST(test_middle_bytes_come_from_the_base_mac);
  return UNITY_END();
}
