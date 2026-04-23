// Copyright 2014 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef PACKAGER_MEDIA_FORMATS_MP4_SINGLE_SEGMENT_SEGMENTER_H_
#define PACKAGER_MEDIA_FORMATS_MP4_SINGLE_SEGMENT_SEGMENTER_H_

#include <cstdint>

#include <packager/file/file_closer.h>
#include <packager/macros/classes.h>
#include <packager/media/event/muxer_listener.h>
#include <packager/media/formats/mp4/segmenter.h>

namespace shaka {
namespace media {
namespace mp4 {

/// Segmenter for MP4 Dash Video-On-Demand profile. A single MP4 file with a
/// single segment is created, i.e. with only one SIDX box. The generated media
/// file can contain one or many subsegments with subsegment duration
/// defined by @b MuxerOptions.segment_duration. A subsegment can contain one
/// or many fragments with fragment duration defined by @b
/// MuxerOptions.fragment_duration. The actual subsegment or fragment duration
/// may not match the requested duration exactly, but will be approximated. That
/// is, the Segmenter tries to end subsegment/fragment at the first sample with
/// overall subsegment/fragment duration not smaller than defined duration and
/// yet meet SAP requirements.
class SingleSegmentSegmenter : public Segmenter {
 public:
  SingleSegmentSegmenter(const MuxerOptions& options,
                         std::unique_ptr<FileType> ftyp,
                         std::unique_ptr<Movie> moov);
  ~SingleSegmentSegmenter() override;

  /// @name Segmenter implementation overrides.
  /// @{
  bool GetInitRange(size_t* offset, size_t* size) override;
  bool GetIndexRange(size_t* offset, size_t* size) override;
  std::vector<Range> GetSegmentRanges() override;
  /// @}

 private:
  // Segmenter implementation overrides.
  Status DoInitialize() override;
  Status DoFinalize() override;
  Status DoFinalizeSegment(int64_t segment_number) override;

  std::unique_ptr<SegmentIndex> vod_sidx_;

  // In-place write path: fragments are written directly to the output file.
  // Used for seekable outputs (local files). moov is pre-written at init with
  // a placeholder duration and patched in-place at finalize. sidx is written
  // into a reserved free-box region so no temp file or second pass is needed.
  bool use_in_place_write_ = false;
  std::unique_ptr<File, FileCloser> output_file_;
  uint64_t moov_offset_ = 0;
  size_t moov_size_at_init_ = 0;
  uint64_t sidx_reserved_offset_ = 0;
  uint64_t media_start_offset_ = 0;

  // Temp-file fallback path: used for non-seekable outputs (e.g. HTTP PUT).
  // Media data is buffered in a temp file during the first pass; DoFinalize
  // writes the final headers then copies the temp data to the output.
  std::string temp_file_name_;
  std::unique_ptr<File, FileCloser> temp_file_;

  DISALLOW_COPY_AND_ASSIGN(SingleSegmentSegmenter);
};

}  // namespace mp4
}  // namespace media
}  // namespace shaka

#endif  // PACKAGER_MEDIA_FORMATS_MP4_SINGLE_SEGMENT_SEGMENTER_H_
