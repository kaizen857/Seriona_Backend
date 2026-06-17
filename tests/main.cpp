#include "doctest.h"

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>

#include "miniaudio.h"

TEST_CASE("baseline placeholder test") {
  CHECK(true);
}

TEST_CASE("ffmpeg dependency discovery baseline") {
  CHECK(AV_VERSION_MAJOR(LIBAVFORMAT_VERSION_INT) >= 0);
  CHECK(AV_VERSION_MAJOR(LIBAVCODEC_VERSION_INT) >= 0);
  CHECK(AV_VERSION_MAJOR(LIBAVUTIL_VERSION_INT) >= 0);
  CHECK(AV_VERSION_MAJOR(LIBAVFILTER_VERSION_INT) >= 0);
  CHECK(AV_VERSION_MAJOR(LIBSWRESAMPLE_VERSION_INT) >= 0);
}
