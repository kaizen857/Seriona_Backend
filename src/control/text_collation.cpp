#include "text_collation.h"

#include <unicode/ucol.h>

#include <cstring>

namespace seriona::control {
namespace {

// ICU 的 UCollator 非线程安全，故每线程持有独立实例（进程内复用，避免每次比较重建）。
struct CollatorHolder {
  UCollator* collator{nullptr};

  CollatorHolder() {
    UErrorCode status = U_ZERO_ERROR;
    // 必须显式 `-u-co-pinyin`：`zh` 默认虽为拼音，但 `zh-Hant` 默认是笔画序。
    collator = ucol_open("zh-u-co-pinyin", &status);
    if (U_FAILURE(status)) {
      collator = nullptr;
      return;
    }
    // SECONDARY：忽略大小写（abc == ABC），区分重音（é != e）。
    ucol_setStrength(collator, UCOL_SECONDARY);
  }

  ~CollatorHolder() {
    if (collator != nullptr) {
      ucol_close(collator);
    }
  }

  CollatorHolder(const CollatorHolder&) = delete;
  CollatorHolder& operator=(const CollatorHolder&) = delete;
};

[[nodiscard]] UCollator* collatorForThread() {
  static thread_local CollatorHolder holder;
  return holder.collator;
}

}

int compareUtf8Bytes(std::string_view left, std::string_view right) {
  const int comparison = left.compare(right);
  if (comparison < 0) {
    return -1;
  }
  if (comparison > 0) {
    return 1;
  }
  return 0;
}

int compareCollatedText(std::string_view left, std::string_view right) {
  UCollator* collator = collatorForThread();
  if (collator == nullptr) {
    // ICU 不可用（打开失败）：退回字节序，行为仍确定。
    return compareUtf8Bytes(left, right);
  }

  UErrorCode status = U_ZERO_ERROR;
  const UCollationResult result = ucol_strcollUTF8(collator,
                                                   left.data(),
                                                   static_cast<int32_t>(left.size()),
                                                   right.data(),
                                                   static_cast<int32_t>(right.size()),
                                                   &status);
  if (U_FAILURE(status)) {
    return compareUtf8Bytes(left, right);
  }
  if (result == UCOL_LESS) {
    return -1;
  }
  if (result == UCOL_GREATER) {
    return 1;
  }
  // collation 视为相等（如仅大小写不同）时以字节序裁决，保证比较是全序、结果确定。
  return compareUtf8Bytes(left, right);
}

}
