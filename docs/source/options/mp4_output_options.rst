MP4 output options
^^^^^^^^^^^^^^^^^^

--mp4_include_pssh_in_stream

    MP4 only: include pssh in the encrypted stream. Default enabled.

--mp4_use_decoding_timestamp_in_timeline

    Deprecated. Do not use.

--generate_sidx_in_media_segments
--nogenerate_sidx_in_media_segments

    Indicates whether to generate 'sidx' box in media segments. Note
    that it is required for DASH on-demand profile (not using segment
    template).

    Default enabled.

--temp_dir <path>

    Specify a directory in which to store the temporary intermediate file
    used during DASH on-demand (single-segment) packaging. The packager
    writes all media fragments to this file first, then copies them to the
    final output alongside the written ``ftyp``, ``moov``, and ``sidx``
    boxes. This requires approximately 2× the output file size in
    temporary disk space.

    Defaults to the system temporary directory (e.g. ``/tmp`` on Linux/macOS).
    Has no effect when ``--mp4_single_pass_vod`` is enabled and the output
    is a seekable local file.

--mp4_single_pass_vod

    MP4 on-demand only: write media fragments directly to the output file
    in a single pass, eliminating the temporary file and the second-pass
    copy that the default two-pass approach requires. Roughly halves peak
    disk usage during packaging.

    Works by pre-writing ``ftyp`` and ``moov`` at the start of the file,
    reserving space for ``sidx`` with a placeholder ``free`` box, and
    patching both in-place at finalize time. Automatically falls back to
    the default two-pass behavior for non-seekable outputs such as HTTP PUT.

    **Only applies to MP4 (ISO-BMFF) container outputs** (``.mp4``, ``.m4a``,
    ``.m4v``, ``.cmfv``, ``.cmfa``). Has no effect for WebM (``.webm``) or any
    other container format — those are unaffected by this flag regardless of
    its value. WebM on-demand packaging always uses its own two-pass approach.

    Not enabled by default. Not recommended for strict CMAF workflows, as
    CMAF validators may reject ``free`` boxes between ``moov`` and the
    first ``moof``.
