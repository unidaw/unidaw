pub fn split_u64(value: u64) -> (u32, u32) {
    (value as u32, (value >> 32) as u32)
}

pub fn unpack_chord_packed(packed: u32) -> (u8, u8, u8, u8) {
    let degree = (packed & 0xff) as u8;
    let quality = ((packed >> 8) & 0xff) as u8;
    let inversion = ((packed >> 16) & 0xff) as u8;
    let base_octave = ((packed >> 24) & 0xff) as u8;
    (degree, quality, inversion, base_octave)
}

pub fn unpack_chord_spread(packed: u32) -> (u32, u8) {
    let column = (packed >> 24) as u8;
    let spread = packed & 0x00ff_ffff;
    (spread, column)
}
