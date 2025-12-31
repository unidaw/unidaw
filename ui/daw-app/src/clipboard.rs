#[derive(Clone, Debug)]
pub struct ClipboardNote {
    pub track: usize,
    pub column: u8,
    pub offset: i64,
    pub pitch: u8,
    pub velocity: u8,
    pub duration: u64,
}

#[derive(Clone, Debug)]
pub struct ClipboardChord {
    pub track: usize,
    pub column: u8,
    pub offset: i64,
    pub duration: u64,
    pub spread: u32,
    pub humanize_timing: u16,
    pub humanize_velocity: u16,
    pub degree: u8,
    pub quality: u8,
    pub inversion: u8,
    pub base_octave: u8,
}

#[derive(Clone, Debug)]
pub struct ClipboardHarmony {
    pub offset: i64,
    pub root: u32,
    pub scale_id: u32,
}

#[derive(Clone, Debug)]
pub struct ClipboardData {
    pub notes: Vec<ClipboardNote>,
    pub chords: Vec<ClipboardChord>,
    pub harmonies: Vec<ClipboardHarmony>,
}
