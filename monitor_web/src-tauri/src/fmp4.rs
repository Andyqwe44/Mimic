/// Minimal fMP4 (fragmented MP4) builder for MSE SourceBuffer.
/// Single Mp4Buf struct, no nested wrapper types. All boxes write to same buffer.

pub struct Mp4Buf {
    pub buf: Vec<u8>,
}

impl Mp4Buf {
    pub fn new() -> Self { Self { buf: Vec::new() } }

    pub fn push_u32(&mut self, v: u32) { self.buf.extend_from_slice(&v.to_be_bytes()); }
    pub fn push_u64(&mut self, v: u64) { self.buf.extend_from_slice(&v.to_be_bytes()); }
    pub fn push_u16(&mut self, v: u16) { self.buf.extend_from_slice(&v.to_be_bytes()); }
    pub fn push_u8(&mut self, v: u8)  { self.buf.push(v); }
    pub fn push_bytes(&mut self, b: &[u8]) { self.buf.extend_from_slice(b); }

    /// Write 3-byte big-endian value (for flags fields)
    pub fn push_u24(&mut self, v: u32) {
        self.buf.extend_from_slice(&[(v >> 16) as u8, (v >> 8) as u8, v as u8]);
    }

    /// Start a box: writes placeholder size + type, returns position for end_box()
    pub fn start_box(&mut self, box_type: &[u8; 4]) -> usize {
        let pos = self.buf.len();
        self.push_u32(0); // placeholder
        self.buf.extend_from_slice(box_type);
        pos
    }

    /// Finish a box: patch size at position
    pub fn end_box(&mut self, pos: usize) {
        let size = (self.buf.len() - pos) as u32;
        self.buf[pos..pos + 4].copy_from_slice(&size.to_be_bytes());
    }

    /// Write a FullBox header (size + type + version + flags), returns position
    pub fn start_fullbox(&mut self, box_type: &[u8; 4], version: u8, flags: u32) -> usize {
        let pos = self.start_box(box_type);
        self.push_u8(version);
        self.push_u24(flags);
        pos
    }
}

// ═══════════════════════════════════════════════════════════
// AVC Decoder Configuration Record
// ═══════════════════════════════════════════════════════════

fn write_avcc(buf: &mut Mp4Buf, sps: &[u8], pps: &[u8]) {
    buf.push_u8(1);      // configurationVersion
    if sps.len() >= 4 {
        buf.push_u8(sps[1]); // profile_idc
        buf.push_u8(sps[2]); // constraint flags
        buf.push_u8(sps[3]); // level_idc
    } else {
        buf.push_bytes(&[0x42, 0xC0, 0x1E]); // Baseline L3.0
    }
    buf.push_u8(0xFF);   // lengthSizeMinusOne=3 (4-byte NAL sizes)
    buf.push_u8(0xE1);   // 1 SPS
    buf.push_u16(sps.len() as u16);
    buf.push_bytes(sps);
    buf.push_u8(0x01);   // 1 PPS
    buf.push_u16(pps.len() as u16);
    buf.push_bytes(pps);
}

// ═══════════════════════════════════════════════════════════
// fMP4 Init Segment
// ═══════════════════════════════════════════════════════════

pub fn build_init_segment(width: u32, height: u32, sps: &[u8], pps: &[u8]) -> Vec<u8> {
    let mut b = Mp4Buf::new();

    // ftyp
    let ftyp = b.start_box(b"ftyp");
    b.push_bytes(b"isom");
    b.push_u32(0x200);
    b.push_bytes(b"isom");
    b.push_bytes(b"iso2");
    b.push_bytes(b"avc1");
    b.push_bytes(b"mp41");
    b.end_box(ftyp);

    // moov
    let moov = b.start_box(b"moov");

    // mvhd
    let mvhd = b.start_fullbox(b"mvhd", 0, 0);
    b.push_u32(0); b.push_u32(0);       // creation/modification time
    b.push_u32(1000);                    // timescale
    b.push_u32(0);                       // duration (0 = fragmented)
    b.push_u32(0x00010000);              // rate 1.0
    b.push_u16(0x0100);                  // volume 1.0
    b.push_bytes(&[0u8; 10]);            // reserved
    // matrix (identity)
    b.push_u32(0x00010000); b.push_u32(0);
    b.push_u32(0); b.push_u32(0x00010000);
    b.push_u32(0); b.push_u32(0);
    b.push_u32(0); b.push_u32(0x40000000);
    b.push_bytes(&[0u8; 24]);            // pre_defined
    b.push_u32(2);                       // next_track_id
    b.end_box(mvhd);

    // trak
    let trak = b.start_box(b"trak");

    // tkhd
    let tkhd = b.start_fullbox(b"tkhd", 0, 0x07);
    b.push_u32(0); b.push_u32(0);       // creation/modification
    b.push_u32(1);                       // track_id
    b.push_u32(0);                       // reserved
    b.push_u32(0);                       // duration
    b.push_bytes(&[0u8; 8]);             // reserved
    b.push_u16(0);                       // layer
    b.push_u16(0);                       // alternate_group
    b.push_u16(0x0100);                  // volume
    b.push_bytes(&[0u8; 2]);             // reserved
    // matrix identity
    b.push_u32(0x00010000); b.push_u32(0);
    b.push_u32(0); b.push_u32(0x00010000);
    b.push_u32(0); b.push_u32(0);
    b.push_u32(0); b.push_u32(0x40000000);
    b.push_u32(width << 16);
    b.push_u32(height << 16);
    b.end_box(tkhd);

    // mdia
    let mdia = b.start_box(b"mdia");

    // mdhd
    let mdhd = b.start_fullbox(b"mdhd", 0, 0);
    b.push_u32(0); b.push_u32(0);       // creation/modification
    b.push_u32(1000);                    // timescale
    b.push_u32(0);                       // duration
    b.push_u16(0x55C4);                  // language (und)
    b.push_u16(0);                       // pre_defined
    b.end_box(mdhd);

    // hdlr
    let hdlr = b.start_fullbox(b"hdlr", 0, 0);
    b.push_bytes(&[0u8; 4]);             // pre_defined
    b.push_bytes(b"vide");               // handler_type
    b.push_bytes(&[0u8; 12]);            // reserved
    b.push_bytes(b"VideoHandler\0");     // name
    b.end_box(hdlr);

    // minf
    let minf = b.start_box(b"minf");

    // vmhd
    let vmhd = b.start_fullbox(b"vmhd", 0, 1);
    b.push_u16(0);                       // graphicsmode
    b.push_bytes(&[0u8; 6]);             // opcolor
    b.end_box(vmhd);

    // dinf → dref → url
    let dinf = b.start_box(b"dinf");
    let dref = b.start_fullbox(b"dref", 0, 0);
    b.push_u32(1);                       // entry_count
    let url = b.start_fullbox(b"url ", 0, 1); // self-contained
    b.end_box(url);
    b.end_box(dref);
    b.end_box(dinf);

    // stbl
    let stbl = b.start_box(b"stbl");

    // stsd
    let stsd = b.start_fullbox(b"stsd", 0, 0);
    b.push_u32(1);                       // entry_count

    // avc1 VisualSampleEntry
    let avc1_start = b.buf.len();
    b.push_u32(0);                       // placeholder size
    b.push_bytes(b"avc1");
    b.push_bytes(&[0u8; 6]);             // reserved
    b.push_u16(1);                       // data_reference_index
    b.push_u16(0);                       // version
    b.push_bytes(&[0u8; 2]);             // revision
    b.push_u32(0);                       // vendor
    b.push_u32(0); b.push_u32(0);        // temporal/spatial quality
    b.push_u16(width as u16);
    b.push_u16(height as u16);
    b.push_u32(0x00480000);              // horizresolution
    b.push_u32(0x00480000);              // vertresolution
    b.push_u32(0);                       // reserved
    b.push_u16(1);                       // frame_count
    // compressorname (32 bytes total)
    let comp_name = b"VideoHandler\0";
    b.push_bytes(comp_name);
    for _ in comp_name.len()..32 { b.push_u8(0); }
    b.push_u16(0x0018);                  // depth
    b.push_u16(0xFFFF);                  // pre_defined

    // avcC
    let avcc_start = b.buf.len();
    b.push_u32(0);                       // placeholder
    b.push_bytes(b"avcC");
    write_avcc(&mut b, sps, pps);
    let avcc_size = (b.buf.len() - avcc_start) as u32;
    b.buf[avcc_start..avcc_start + 4].copy_from_slice(&avcc_size.to_be_bytes());

    // Fix avc1 entry size
    let avc1_size = (b.buf.len() - avc1_start) as u32;
    b.buf[avc1_start..avc1_start + 4].copy_from_slice(&avc1_size.to_be_bytes());
    b.end_box(stsd);

    // stts, stsc, stsz, stco (all empty for fragmented)
    {
        let stts = b.start_fullbox(b"stts", 0, 0);
        b.push_u32(0); // entry_count
        b.end_box(stts);

        let stsc = b.start_fullbox(b"stsc", 0, 0);
        b.push_u32(0); // entry_count
        b.end_box(stsc);

        let stsz = b.start_fullbox(b"stsz", 0, 0);
        b.push_u32(0); // sample_size
        b.push_u32(0); // sample_count
        b.end_box(stsz);
    }
    let stco = b.start_fullbox(b"stco", 0, 0);
    b.push_u32(0);
    b.end_box(stco);

    b.end_box(stbl);
    b.end_box(minf);
    b.end_box(mdia);
    b.end_box(trak);
    b.end_box(moov);

    // mvex
    let mvex = b.start_box(b"mvex");
    let mehd = b.start_fullbox(b"mehd", 0, 0);
    b.push_u32(0); // fragment_duration
    b.end_box(mehd);
    let trex = b.start_fullbox(b"trex", 0, 0);
    b.push_u32(1); b.push_u32(1); // track_id, default_sample_desc_index
    b.push_u32(0); b.push_u32(0); b.push_u32(0); // duration, size, flags
    b.end_box(trex);
    b.end_box(mvex);

    b.buf
}

// ═══════════════════════════════════════════════════════════
// fMP4 Media Segment (moof + mdat) for 1 H.264 frame
// ═══════════════════════════════════════════════════════════

pub fn build_media_segment(
    h264_data: &[u8],
    sequence_number: u32,
    base_decode_time: u64,
    sample_duration: u32,
) -> Vec<u8> {
    let mut b = Mp4Buf::new();

    // Convert Annex B → length-prefixed NAL units for mdat
    let mut nal_data = Vec::with_capacity(h264_data.len());
    let mut pos = 0;
    while pos < h264_data.len() {
        // Skip start code
        if pos + 4 <= h264_data.len()
            && h264_data[pos..pos + 4] == [0x00, 0x00, 0x00, 0x01]
        {
            pos += 4;
        } else if pos + 3 <= h264_data.len()
            && h264_data[pos..pos + 3] == [0x00, 0x00, 0x01]
        {
            pos += 3;
        } else {
            let remaining = h264_data.len() - pos;
            nal_data.extend_from_slice(&(remaining as u32).to_be_bytes());
            nal_data.extend_from_slice(&h264_data[pos..]);
            break;
        }

        let mut nal_end = pos;
        while nal_end + 3 <= h264_data.len() {
            if (h264_data[nal_end..nal_end + 3] == [0x00, 0x00, 0x01])
                || (nal_end + 4 <= h264_data.len()
                    && h264_data[nal_end..nal_end + 4] == [0x00, 0x00, 0x00, 0x01])
            {
                break;
            }
            nal_end += 1;
        }
        let nal_len = nal_end - pos;
        nal_data.extend_from_slice(&(nal_len as u32).to_be_bytes());
        nal_data.extend_from_slice(&h264_data[pos..nal_end]);
        pos = nal_end;
    }

    let sample_size = nal_data.len() as u32;

    // moof — size = 100 bytes for 1-sample case
    let moof = b.start_box(b"moof");

    // mfhd
    let mfhd = b.start_fullbox(b"mfhd", 0, 0);
    b.push_u32(sequence_number);
    b.end_box(mfhd);

    // traf
    let traf = b.start_box(b"traf");

    // tfhd: default-base-is-moof flag = 0x020000
    let tfhd = b.start_fullbox(b"tfhd", 0, 0x020000);
    b.push_u32(1); // track_id
    b.end_box(tfhd);

    // tfdt (version 1 = 64-bit time)
    let tfdt = b.start_fullbox(b"tfdt", 1, 0);
    b.push_u64(base_decode_time);
    b.end_box(tfdt);

    // trun: 1 sample, flags = data_offset + duration + size + sample_flags
    let data_offset = 108u32; // moof(100) + mdat header(8)
    let flags: u32 = 0x000001 | 0x000100 | 0x000200 | 0x000400;
    let is_keyframe = sequence_number == 1;
    let sample_flags: u32 = if is_keyframe { 0x02000000 } else { 0x01010000 };
    let trun = b.start_fullbox(b"trun", 1, flags);
    b.push_u32(1);               // sample_count
    b.push_u32(data_offset);
    b.push_u32(sample_duration);  // sample.duration
    b.push_u32(sample_size);      // sample.size
    b.push_u32(sample_flags);     // sample.flags
    b.end_box(trun);

    b.end_box(traf);
    b.end_box(moof);  // patched to correct size

    // mdat
    let mdat = b.start_box(b"mdat");
    b.push_bytes(&nal_data);
    b.end_box(mdat);

    b.buf
}
