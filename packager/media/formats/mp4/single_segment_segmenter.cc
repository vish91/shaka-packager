// Copyright 2014 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/formats/mp4/single_segment_segmenter.h>

#include <algorithm>
#include <vector>

#include <absl/log/check.h>
#include <absl/log/log.h>

#include <packager/file/file_util.h>
#include <packager/media/base/buffer_writer.h>
#include <packager/media/base/fourccs.h>
#include <packager/media/base/muxer_options.h>
#include <packager/media/event/progress_listener.h>
#include <packager/media/formats/mp4/key_frame_info.h>

namespace shaka {
namespace media {
namespace mp4 {

namespace {

// Reserved space after moov to absorb size changes at finalize time.
// moov size can grow by up to 4 bytes if mehd.fragment_duration overflows
// 32 bits (i.e. content longer than ~13 hours at 90 kHz timescale).
// 16 bytes gives a valid free box with comfortable margin.
constexpr size_t kMoovCushionSize = 16;

// Reserved space for the sidx box. sidx is 20 bytes + 12 bytes per subsegment
// reference. 64 KB covers ~5450 subsegments (roughly 3 hours at 2s segments
// or 15 hours at 10s segments).
constexpr size_t kReservedSidxSize = 64 * 1024;

// Writes an ISO BMFF 'free' box of exactly |size| bytes (size >= 8).
Status WriteFreeBox(File* file, size_t size) {
  DCHECK_GE(size, 8u);
  BufferWriter bw;
  bw.AppendInt(static_cast<uint32_t>(size));
  bw.AppendInt(static_cast<uint32_t>(FOURCC_free));
  std::vector<uint8_t> zeros(size - 8, 0);
  bw.AppendArray(zeros.data(), zeros.size());
  return bw.WriteToFile(file);
}

}  // namespace

SingleSegmentSegmenter::SingleSegmentSegmenter(const MuxerOptions& options,
                                               std::unique_ptr<FileType> ftyp,
                                               std::unique_ptr<Movie> moov)
    : Segmenter(options, std::move(ftyp), std::move(moov)) {}

SingleSegmentSegmenter::~SingleSegmentSegmenter() {
  if (output_file_)
    output_file_.release()->Close();
  if (temp_file_)
    temp_file_.release()->Close();
  if (!temp_file_name_.empty()) {
    if (!File::Delete(temp_file_name_.c_str()))
      LOG(ERROR) << "Unable to delete temporary file " << temp_file_name_;
  }
}

bool SingleSegmentSegmenter::GetInitRange(size_t* offset, size_t* size) {
  *offset = 0;
  *size = ftyp()->ComputeSize() + moov()->ComputeSize();
  return true;
}

bool SingleSegmentSegmenter::GetIndexRange(size_t* offset, size_t* size) {
  if (use_in_place_write_) {
    // sidx was written into the reserved region, whose start is known.
    *offset = sidx_reserved_offset_;
  } else {
    *offset = ftyp()->ComputeSize() + moov()->ComputeSize();
  }
  *size = options().mp4_params.generate_sidx_in_media_segments
              ? vod_sidx_->ComputeSize()
              : 0;
  return true;
}

std::vector<Range> SingleSegmentSegmenter::GetSegmentRanges() {
  std::vector<Range> ranges;
  uint64_t next_offset;
  if (use_in_place_write_) {
    // media_start_offset_ is the absolute position of the first moof, computed
    // at DoInitialize() as sidx_reserved_offset_ + kReservedSidxSize. This
    // already equals sidx_box_end + first_offset (the terms cancel), so no
    // additional adjustment for first_offset is needed here.
    next_offset = media_start_offset_;
  } else {
    next_offset = ftyp()->ComputeSize() + moov()->ComputeSize() +
                  (options().mp4_params.generate_sidx_in_media_segments
                       ? vod_sidx_->ComputeSize()
                       : 0) +
                  vod_sidx_->first_offset;
  }
  for (const SegmentReference& ref : vod_sidx_->references) {
    Range r;
    r.start = next_offset;
    r.end = r.start + ref.referenced_size - 1;  // ranges are inclusive
    next_offset = r.end + 1;
    ranges.push_back(r);
  }
  return ranges;
}

Status SingleSegmentSegmenter::DoInitialize() {
  // Open the output file and probe whether it supports seeking. Local files
  // do; non-seekable outputs like HTTP PUT do not.
  output_file_.reset(File::Open(options().output_file_name.c_str(), "w"));
  if (!output_file_) {
    return Status(error::FILE_FAILURE,
                  "Cannot open file to write " + options().output_file_name);
  }

  // Only attempt the single-pass in-place approach when explicitly requested
  // via --mp4_single_pass_vod and the output is seekable (local files).
  // Non-seekable outputs (HTTP PUT etc.) always fall back to the two-pass
  // approach. When the flag is not set, use two-pass unconditionally so that
  // CMAF pipelines and other strict workflows are unaffected.
  uint64_t probe_pos = 0;
  use_in_place_write_ = options().mp4_params.single_pass_vod &&
                        output_file_->Tell(&probe_pos);

  if (!use_in_place_write_) {
    // Two-pass path: either the flag is off, or the output is not seekable.
    // Close the prematurely-opened output file (it will be re-opened in
    // DoFinalize after the temp file has been fully written).
    output_file_.release()->Close();
    output_file_.reset();

    // Double progress target to account for the second-pass copy in DoFinalize.
    set_progress_target(progress_target() * 2);

    if (!TempFilePath(options().temp_dir, &temp_file_name_))
      return Status(error::FILE_FAILURE, "Unable to create temporary file.");
    temp_file_.reset(File::Open(temp_file_name_.c_str(), "w"));
    return temp_file_ ? Status::OK
                      : Status(error::FILE_FAILURE,
                               "Cannot open file to write " + temp_file_name_);
  }

  // Seekable output path: pre-write ftyp + moov (with placeholder duration=0)
  // so media fragments can be appended directly during the first (and only)
  // pass. At finalize, we seek back to patch the moov duration and write the
  // real sidx into the reserved region. No temp file, no second-pass copy.
  //
  // For fragmented MP4 (used in DASH on-demand), the moov box contains no
  // per-sample tables (stts/stsz/stco are empty), so its size is fully
  // determined at initialization time from codec parameters alone.

  moov_offset_ = static_cast<uint64_t>(ftyp()->ComputeSize());

  // Write ftyp.
  {
    BufferWriter bw;
    ftyp()->Write(&bw);
    Status s = bw.WriteToFile(output_file_.get());
    if (!s.ok())
      return s;
  }

  // Write moov with a placeholder fragment_duration of 1.
  //
  // MovieExtendsHeader (mehd) has ComputeSizeInternal() == 0 when
  // fragment_duration == 0, meaning the box is entirely omitted. At finalize,
  // the real duration is non-zero, causing mehd to appear (+16 bytes). Setting
  // duration=1 here ensures mehd is present and the serialized moov size is
  // stable. The value is overwritten in-place at finalize.
  moov()->extends.header.fragment_duration = 1;
  moov_size_at_init_ = moov()->ComputeSize();
  {
    BufferWriter bw;
    moov()->Write(&bw);
    Status s = bw.WriteToFile(output_file_.get());
    if (!s.ok())
      return s;
  }
  // Reset to 0 so Segmenter::Finalize() computes and sets the real value.
  moov()->extends.header.fragment_duration = 0;

  // Write a small free box after moov to absorb any moov size growth at
  // finalize. With the placeholder above, moov size is stable for all typical
  // content. The only remaining source of growth is a version 0→1 flip in
  // mehd when fragment_duration exceeds 32 bits (content > ~13 hours at
  // 90 kHz timescale), which adds 4 bytes. kMoovCushionSize=16 gives a valid
  // free box (min 8 bytes) with comfortable margin.
  {
    Status s = WriteFreeBox(output_file_.get(), kMoovCushionSize);
    if (!s.ok())
      return s;
  }

  sidx_reserved_offset_ = moov_offset_ + moov_size_at_init_ + kMoovCushionSize;
  media_start_offset_ = sidx_reserved_offset_;

  if (options().mp4_params.generate_sidx_in_media_segments) {
    // Reserve space for the sidx box using a free box placeholder. The real
    // sidx is written here at finalize. sidx.first_offset will account for
    // any gap between the end of the actual sidx and the first moof.
    Status s = WriteFreeBox(output_file_.get(), kReservedSidxSize);
    if (!s.ok())
      return s;
    media_start_offset_ += kReservedSidxSize;
  }

  return Status::OK;
}

Status SingleSegmentSegmenter::DoFinalize() {
  DCHECK(ftyp());
  DCHECK(moov());
  DCHECK(vod_sidx_);

  if (!use_in_place_write_) {
    // Two-pass temp-file path (non-seekable outputs).
    DCHECK(temp_file_);

    if (!temp_file_.release()->Close()) {
      return Status(
          error::FILE_FAILURE,
          "Cannot close the temp file " + temp_file_name_ +
              ", possibly file permission issue or running out of disk space.");
    }

    std::unique_ptr<File, FileCloser> file(
        File::Open(options().output_file_name.c_str(), "w"));
    if (!file) {
      return Status(error::FILE_FAILURE,
                    "Cannot open file to write " + options().output_file_name);
    }

    LOG(INFO) << "Update media header (moov) and rewrite the file to '"
              << options().output_file_name << "'.";

    BufferWriter buffer;
    ftyp()->Write(&buffer);
    moov()->Write(&buffer);
    if (options().mp4_params.generate_sidx_in_media_segments)
      vod_sidx_->Write(&buffer);
    Status status = buffer.WriteToFile(file.get());
    if (!status.ok())
      return status;

    std::unique_ptr<File, FileCloser> temp_file(
        File::Open(temp_file_name_.c_str(), "r"));
    if (!temp_file) {
      return Status(error::FILE_FAILURE,
                    "Cannot open file to read " + temp_file_name_);
    }

    const uint64_t re_segment_progress_target = progress_target() * 0.5;
    const int kBufSize = 0x200000;  // 2MB
    std::unique_ptr<uint8_t[]> buf(new uint8_t[kBufSize]);
    while (true) {
      int64_t size = temp_file->Read(buf.get(), kBufSize);
      if (size == 0) {
        break;
      } else if (size < 0) {
        return Status(error::FILE_FAILURE,
                      "Failed to read file " + temp_file_name_);
      }
      if (file->Write(buf.get(), size) != size) {
        return Status(error::FILE_FAILURE,
                      "Failed to write file " + options().output_file_name);
      }
      UpdateProgress(static_cast<double>(size) / temp_file->Size() *
                     re_segment_progress_target);
    }
    if (!temp_file.release()->Close()) {
      return Status(error::FILE_FAILURE, "Cannot close the temp file " +
                                             temp_file_name_ + " after reading.");
    }
    if (!file.release()->Close()) {
      return Status(
          error::FILE_FAILURE,
          "Cannot close file " + options().output_file_name +
              ", possibly file permission issue or running out of disk space.");
    }
    SetComplete();
    return Status::OK;
  }

  // In-place write path: seek back and patch moov, then write sidx into the
  // reserved region. No temp file, no second-pass copy.
  DCHECK(output_file_);

  // Verify that moov size is stable (it should be for fragmented MP4, since
  // stbl tables are empty and the only variable field is mehd.fragment_duration
  // which is a fixed-width integer regardless of value for typical content).
  const size_t moov_size_final = moov()->ComputeSize();
  const int64_t moov_size_delta =
      static_cast<int64_t>(moov_size_final) -
      static_cast<int64_t>(moov_size_at_init_);
  const int64_t cushion_remaining =
      static_cast<int64_t>(kMoovCushionSize) - moov_size_delta;

  if (cushion_remaining < 8) {
    // moov grew more than the cushion can absorb (content > ~13 hours at
    // 90 kHz timescale). This is extremely rare in practice.
    return Status(
        error::FILE_FAILURE,
        "moov box grew by " + std::to_string(moov_size_delta) +
            " bytes at finalize, exceeding the reserved cushion of " +
            std::to_string(kMoovCushionSize) + " bytes. This can occur for "
            "content longer than ~13 hours at 90 kHz timescale.");
  }

  // Seek to moov position and overwrite with the finalized moov.
  if (!output_file_->Seek(moov_offset_)) {
    return Status(error::FILE_FAILURE,
                  "Failed to seek to moov position in " +
                      options().output_file_name);
  }
  {
    BufferWriter bw;
    moov()->Write(&bw);
    Status s = bw.WriteToFile(output_file_.get());
    if (!s.ok())
      return s;
  }
  // Overwrite the cushion free box with the (possibly shrunken) remainder.
  {
    Status s = WriteFreeBox(output_file_.get(),
                            static_cast<size_t>(cushion_remaining));
    if (!s.ok())
      return s;
  }

  // Write the real sidx into the reserved region.
  if (options().mp4_params.generate_sidx_in_media_segments) {
    // Compute how much of the reserved region the sidx will occupy. The
    // remainder becomes the sidx.first_offset so the segment index correctly
    // points to the first moof regardless of how much space is unused.
    const size_t sidx_size = vod_sidx_->ComputeSize();

    if (sidx_size > kReservedSidxSize) {
      return Status(
          error::FILE_FAILURE,
          "sidx box (" + std::to_string(sidx_size) +
              " bytes) exceeds the reserved region (" +
              std::to_string(kReservedSidxSize) +
              " bytes). Content has too many subsegments for the default "
              "reservation. Increase kReservedSidxSize or use larger segment "
              "durations.");
    }

    const size_t sidx_gap = kReservedSidxSize - sidx_size;
    // first_offset: distance from the first byte after the sidx box to the
    // first byte of the first referenced moof. This accounts for the trailing
    // gap (which is either a free box or, if < 8 bytes, raw padding).
    vod_sidx_->first_offset = static_cast<uint64_t>(sidx_gap);

    if (!output_file_->Seek(sidx_reserved_offset_)) {
      return Status(error::FILE_FAILURE,
                    "Failed to seek to sidx reserved region in " +
                        options().output_file_name);
    }
    {
      BufferWriter bw;
      vod_sidx_->Write(&bw);
      Status s = bw.WriteToFile(output_file_.get());
      if (!s.ok())
        return s;
    }

    // Fill the gap with a free box if there's enough room for a valid one,
    // otherwise write raw zeros so the file is at least byte-correct.
    if (sidx_gap >= 8) {
      Status s = WriteFreeBox(output_file_.get(), sidx_gap);
      if (!s.ok())
        return s;
    } else if (sidx_gap > 0) {
      std::vector<uint8_t> zeros(sidx_gap, 0);
      if (output_file_->Write(zeros.data(), sidx_gap) !=
          static_cast<int64_t>(sidx_gap)) {
        return Status(error::FILE_FAILURE,
                      "Failed to write sidx gap padding in " +
                          options().output_file_name);
      }
    }
  }

  if (!output_file_.release()->Close()) {
    return Status(
        error::FILE_FAILURE,
        "Cannot close file " + options().output_file_name +
            ", possibly file permission issue or running out of disk space.");
  }
  SetComplete();
  return Status::OK;
}

Status SingleSegmentSegmenter::DoFinalizeSegment(int64_t segment_number) {
  DCHECK(sidx());
  DCHECK(fragment_buffer());
  // sidx() contains pre-generated segment references with one reference per
  // fragment. In VOD, this segment is converted into a subsegment, i.e. one
  // reference, which contains all the fragments in sidx().
  std::vector<SegmentReference>& refs = sidx()->references;
  SegmentReference& vod_ref = refs[0];
  int64_t first_sap_time =
      refs[0].sap_delta_time + refs[0].earliest_presentation_time;
  for (uint32_t i = 1; i < refs.size(); ++i) {
    vod_ref.referenced_size += refs[i].referenced_size;
    // NOTE: We calculate subsegment duration based on the total duration of
    // this subsegment instead of subtracting earliest_presentation_time as
    // indicated in the spec.
    vod_ref.subsegment_duration += refs[i].subsegment_duration;
    vod_ref.earliest_presentation_time = std::min(
        vod_ref.earliest_presentation_time, refs[i].earliest_presentation_time);

    if (vod_ref.sap_type == SegmentReference::TypeUnknown &&
        refs[i].sap_type != SegmentReference::TypeUnknown) {
      vod_ref.sap_type = refs[i].sap_type;
      first_sap_time =
          refs[i].sap_delta_time + refs[i].earliest_presentation_time;
    }
  }
  // Calculate sap delta time w.r.t. earliest_presentation_time.
  if (vod_ref.sap_type != SegmentReference::TypeUnknown) {
    vod_ref.sap_delta_time =
        first_sap_time - vod_ref.earliest_presentation_time;
  }

  // Create segment if it does not exist yet.
  if (!vod_sidx_) {
    vod_sidx_.reset(new SegmentIndex());
    vod_sidx_->reference_id = sidx()->reference_id;
    vod_sidx_->timescale = sidx()->timescale;
    vod_sidx_->earliest_presentation_time = vod_ref.earliest_presentation_time;
  }
  vod_sidx_->references.push_back(vod_ref);

  if (muxer_listener()) {
    for (const KeyFrameInfo& key_frame_info : key_frame_infos()) {
      // Unlike multisegment-segmenter, there is no (sub)segment header (styp,
      // sidx), so this is already the offset within the (sub)segment.
      muxer_listener()->OnKeyFrame(key_frame_info.timestamp,
                                   key_frame_info.start_byte_offset,
                                   key_frame_info.size);
    }
  }

  // Write fragment buffer to the appropriate sink.
  size_t segment_size = fragment_buffer()->Size();
  File* sink = use_in_place_write_ ? output_file_.get() : temp_file_.get();
  Status status = fragment_buffer()->WriteToFile(sink);
  if (!status.ok())
    return status;

  UpdateProgress(vod_ref.subsegment_duration);
  if (muxer_listener()) {
    muxer_listener()->OnSampleDurationReady(sample_duration());
    muxer_listener()->OnNewSegment(
        options().output_file_name, vod_ref.earliest_presentation_time,
        vod_ref.subsegment_duration, segment_size, segment_number);
  }
  return Status::OK;
}

}  // namespace mp4
}  // namespace media
}  // namespace shaka
