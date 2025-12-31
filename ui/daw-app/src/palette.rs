#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PaletteMode {
    Commands,
    Plugins,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PaletteCommandId {
    LoadPlugin,
    SetHarmonyScale,
}

#[derive(Clone, Copy, Debug)]
pub struct PaletteCommand {
    pub id: PaletteCommandId,
    pub label: &'static str,
    pub hint: &'static str,
}

pub const PALETTE_COMMANDS: &[PaletteCommand] = &[
    PaletteCommand {
        id: PaletteCommandId::LoadPlugin,
        label: "Load Plugin on Track…",
        hint: "Enter",
    },
    PaletteCommand {
        id: PaletteCommandId::SetHarmonyScale,
        label: "Set Harmony Scale…",
        hint: "Cmd+Shift+S",
    },
];
