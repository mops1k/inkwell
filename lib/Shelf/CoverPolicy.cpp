#include "CoverPolicy.h"

namespace shelf {

uint32_t fatTimeToSeconds(uint32_t fatDateTime) {
  if (fatDateTime == 0) return 0;
  const uint32_t date = (fatDateTime >> 16) & 0xFFFF;
  const uint32_t time = fatDateTime & 0xFFFF;
  const uint32_t year = 1980 + (date >> 9);
  const uint32_t month = (date >> 5) & 0x0F;
  const uint32_t day = date & 0x1F;
  const uint32_t hour = (time >> 11) & 0x1F;
  const uint32_t minute = (time >> 5) & 0x3F;
  const uint32_t second = (time & 0x1F) * 2;
  // A month length of 31 and a year length of 372 are just a monotonic
  // linearisation of the three fields: ordering is what matters, not the value.
  const uint32_t days = year * 372 + month * 31 + day;
  return days * 86400 + hour * 3600 + minute * 60 + second;
}

bool coverIsStale(uint32_t bookFatDateTime, uint32_t thumbFatDateTime) {
  if (bookFatDateTime == 0 || thumbFatDateTime == 0) return false;
  return fatTimeToSeconds(thumbFatDateTime) + kCoverStaleSlackSeconds < fatTimeToSeconds(bookFatDateTime);
}

CoverDecision decideCoverAction(uint32_t bookFatDateTime, const CoverThumbState& thumb) {
  if (thumb.sentinel) return CoverDecision::Placeholder;
  if (!thumb.exists || !thumb.complete) return CoverDecision::Generate;
  if (coverIsStale(bookFatDateTime, thumb.fatDateTime)) return CoverDecision::Generate;
  return CoverDecision::Reuse;
}

CoverDecision decideAfterFailure(ThumbResult result, uint8_t transientAttempts) {
  switch (result) {
    case ThumbResult::Ok:
      return CoverDecision::Reuse;
    case ThumbResult::StructurallyAbsent:
      return CoverDecision::Placeholder;
    case ThumbResult::TransientFail:
      // The first failure retries later; the second one settles on a
      // placeholder so a flaky decoder can never spin forever.
      return transientAttempts == 0 ? CoverDecision::RetryLater : CoverDecision::Placeholder;
  }
  return CoverDecision::RetryLater;
}

}  // namespace shelf
