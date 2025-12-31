#[derive(Clone, Copy, Debug)]
pub struct ScaleInfo {
    pub id: u32,
    pub name: &'static str,
    pub key: &'static str,
}

pub const SCALE_LIBRARY: &[ScaleInfo] = &[
    ScaleInfo { id: 1, name: "maj", key: "1" },
    ScaleInfo { id: 2, name: "min", key: "2" },
    ScaleInfo { id: 3, name: "dor", key: "3" },
    ScaleInfo { id: 4, name: "mix", key: "4" },
];

pub fn harmony_root_name(root: u32) -> &'static str {
    match root % 12 {
        0 => "C",
        1 => "C#",
        2 => "D",
        3 => "D#",
        4 => "E",
        5 => "F",
        6 => "F#",
        7 => "G",
        8 => "G#",
        9 => "A",
        10 => "A#",
        11 => "B",
        _ => "C",
    }
}

pub fn harmony_scale_name(scale_id: u32) -> &'static str {
    for scale in SCALE_LIBRARY {
        if scale.id == scale_id {
            return scale.name;
        }
    }
    "chr"
}
