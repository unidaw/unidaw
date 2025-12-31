#[derive(Clone, Copy, Debug)]
pub struct SelectionRange {
    pub start: u64,
    pub end: u64,
}

#[derive(Clone, Debug)]
pub struct SelectionMask {
    pub tracks: Vec<u8>,
    pub harmony: bool,
}

impl SelectionMask {
    pub fn empty(track_count: usize) -> Self {
        Self {
            tracks: vec![0; track_count],
            harmony: false,
        }
    }
}
