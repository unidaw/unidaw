#pragma once

#include "apps/event_payloads.h"

namespace daw::engine {

// DOES THIS COMMAND CHANGE THE AUTHORED DOCUMENT?
//
// THE SWITCH HAS NO `default:` LABEL, and that is the entire point. A new UiCommandType will not
// compile until somebody decides whether it is undoable — which is the failure this whole effort
// is about, arriving as a build error instead of as a bug report a year later. The same device
// apps/device_chain.h uses to make -Wswitch report an unhandled DeviceKind.
//
// `false` means the command changes nothing that is SAVED: queries, transport, auditioning,
// window opening, the save/load verbs themselves, and SetModSourceValue (modSources is never
// serialised). Every one of those was checked against the audit of 2026-08-06 rather than
// assumed, and a wrong `false` here is invisible — it silently removes a command from undo — so
// the list is short and each entry is defensible on its own.
// WHAT A COMMAND DOES TO THE HISTORY — three answers, because two were not enough.
//
// `commandMutatesDocument` is a bool, and a bool cannot say "this changes the document but must
// not open an undo step". That state is not hypothetical: the A/B audition swap is built so a
// musician can flip between their take and an agent's draft, and scratch_clip_check has asserted
// since the feature shipped that auditioning does NOT consume the undo stack — "the agent's draft
// standing between you and your own edit history is exactly what this feature exists to prevent".
// The switchover made every mutating command undoable, which is right, and thereby broke that:
// after two auditions, Ctrl-Z reversed a SWAP instead of the note the user had typed.
//
// THE LINE IS WHAT THE COMMAND CHANGES, not how it feels:
//   Version — it changes what EXISTS. Fork creates a draft; ClearPlacementAlternate destroys one.
//             Losing a draft to a mis-click must be reversible, so both open a step.
//   Amend   — it changes only WHICH OF THEM YOU ARE HEARING. Swap moves no notes and loses no
//             work. It rewrites the version at the cursor in place, so the history stays in sync
//             with the live document without growing a step the user never made.
//   None    — it changes nothing that is saved.
//
// Amend is NOT "skip the recording". Skipping would leave the cursor's version holding a stale
// audition state, and the next undo would silently flip the placement back as collateral. The
// document still has to be captured; what changes is whether a new step appears.
enum class UndoPolicy { None, Version, Amend };

// The audition swap is the only Amend today. Kept as its own function rather than a second
// switch-complete classification, so there is one place that decides and nothing to drift apart.
constexpr UndoPolicy commandUndoPolicy(daw::UiCommandType type);

constexpr bool commandMutatesDocument(daw::UiCommandType type) {
  switch (type) {
    case daw::UiCommandType::None:  // no document state
      return false;
    case daw::UiCommandType::LoadPluginOnTrack:
      return true;
    case daw::UiCommandType::WriteNote:
      return true;
    case daw::UiCommandType::TogglePlay:  // no document state
      return false;
    case daw::UiCommandType::DeleteNote:
      return true;
    case daw::UiCommandType::Undo:  // no document state
      return false;
    case daw::UiCommandType::WriteHarmony:
      return true;
    case daw::UiCommandType::DeleteHarmony:
      return true;
    case daw::UiCommandType::WriteChord:
      return true;
    case daw::UiCommandType::DeleteChord:
      return true;
    case daw::UiCommandType::SetTrackHarmonyQuantize:
      return true;
    case daw::UiCommandType::Redo:  // no document state
      return false;
    case daw::UiCommandType::SetLoopRange:  // no document state
      return false;
    case daw::UiCommandType::SetAutomationTarget:
      return true;
    case daw::UiCommandType::AddDevice:
      return true;
    case daw::UiCommandType::RemoveDevice:
      return true;
    case daw::UiCommandType::MoveDevice:
      return true;
    case daw::UiCommandType::UpdateDevice:
      return true;
    case daw::UiCommandType::SetDeviceEuclideanConfig:
      return true;
    case daw::UiCommandType::SetTrackRouting:
      return true;
    case daw::UiCommandType::AddModLink:
      return true;
    case daw::UiCommandType::RemoveModLink:
      return true;
    case daw::UiCommandType::SetModLinkUid16:
      return true;
    case daw::UiCommandType::SetModSourceValue:  // no document state
      return false;
    case daw::UiCommandType::OpenPluginEditor:  // no document state
      return false;
    case daw::UiCommandType::AddPatcherNode:
      return true;
    case daw::UiCommandType::RemovePatcherNode:
      return true;
    case daw::UiCommandType::ConnectPatcherNodes:
      return true;
    case daw::UiCommandType::SetPatcherNodeConfig:
      return true;
    case daw::UiCommandType::SavePatcherPreset:  // no document state
      return false;
    case daw::UiCommandType::RequestClipWindow:  // no document state
      return false;
    case daw::UiCommandType::SaveProject:  // no document state
      return false;
    case daw::UiCommandType::LoadProject:  // no document state
      return false;
    case daw::UiCommandType::SetTrackMixer:
      return true;
    case daw::UiCommandType::Stop:  // no document state
      return false;
    case daw::UiCommandType::SetPosition:  // no document state
      return false;
    case daw::UiCommandType::SetTrackName:
      return true;
    case daw::UiCommandType::RequestChainSnapshot:  // no document state
      return false;
    case daw::UiCommandType::RequestDeviceParams:  // no document state
      return false;
    case daw::UiCommandType::SetTempo:
      return true;
    case daw::UiCommandType::Quit:  // no document state
      return false;
    case daw::UiCommandType::SetDeviceParam:
      return true;
    case daw::UiCommandType::RequestWaveform:  // no document state
      return false;
    case daw::UiCommandType::PreviewNote:  // no document state
      return false;
    case daw::UiCommandType::AddTrack:
      return true;
    case daw::UiCommandType::RemoveTrack:
      return true;
    case daw::UiCommandType::MovePlacement:
      return true;
    case daw::UiCommandType::RemovePlacement:
      return true;
    case daw::UiCommandType::ResizePlacement:
      return true;
    case daw::UiCommandType::AddPlacement:
      return true;
    case daw::UiCommandType::Panic:  // no document state
      return false;
    case daw::UiCommandType::SetLaneQuantize:
      return true;
    case daw::UiCommandType::RevertPlacementOverrides:
      return true;
    case daw::UiCommandType::WriteAutomationPoint:
      return true;
    case daw::UiCommandType::SetPlacementEditScope:
      return true;
    case daw::UiCommandType::RequestAutomationLane:  // no document state
      return false;
    case daw::UiCommandType::SetModLinkDepth:
      return true;
    case daw::UiCommandType::AddMarker:
      return true;
    case daw::UiCommandType::RemoveMarker:
      return true;
    case daw::UiCommandType::RenameMarker:
      return true;
    case daw::UiCommandType::MoveMarker:
      return true;
    case daw::UiCommandType::SetTimeSignature:
      return true;
    case daw::UiCommandType::InsertRemoveTime:
      return true;
    case daw::UiCommandType::ForkPlacementClip:
      return true;
    case daw::UiCommandType::SwapPlacementClip:
      return true;
    case daw::UiCommandType::ClearPlacementAlternate:
      return true;
    case daw::UiCommandType::SamplerLoad:
      return true;
    case daw::UiCommandType::SamplerSetSlot:
      return true;
    case daw::UiCommandType::RequestSamplerKit:  // no document state
      return false;
    case daw::UiCommandType::SamplerSlice:
      return true;
    case daw::UiCommandType::SamplerMarker:
      return true;
    case daw::UiCommandType::SamplerEmitRows:
      return true;
    case daw::UiCommandType::SaveModule:  // no document state
      return false;
    case daw::UiCommandType::LoadModule:  // no document state
      return false;
    case daw::UiCommandType::SetRowOps:
      return true;
    case daw::UiCommandType::SamplerSetEnvelope:
      return true;
    case daw::UiCommandType::BulkChunk:  // no document state
      return false;
    case daw::UiCommandType::SamplerSetEnvelopePoints:
      return true;
    case daw::UiCommandType::SamplerSetLfo:
      return true;
    case daw::UiCommandType::SamplerSetFilter:
      return true;
    case daw::UiCommandType::SetTrackSoundAddressed:
      return true;
    case daw::UiCommandType::SamplerSetDevice:
      return true;
    case daw::UiCommandType::SetTrackCollapsed:
      return true;
    case daw::UiCommandType::SamplerSetSlotName:
      return true;
    case daw::UiCommandType::SamplerSetVintage:
      return true;
    case daw::UiCommandType::SetTrackLinesPerBeat:
      return true;
    case daw::UiCommandType::SetTrackAllowNoteOverlap:
      return true;
    case daw::UiCommandType::SetClipGrid:
      return true;
    case daw::UiCommandType::SetAudioClipField:
      return true;
    case daw::UiCommandType::DeleteAutomationPoint:
      return true;
    case daw::UiCommandType::RequestSamplerEnvelope:  // no document state
      return false;
    case daw::UiCommandType::SetClipText:
      return true;
    case daw::UiCommandType::SetMarkerColor:
      return true;
  }
  return true;  // unreachable for a complete switch; a hostile cast lands here and is recorded
}

// WHAT THE UNDO MENU SAYS. DocumentHistory::commit requires a label, so every command has one and
// none can be recorded anonymously — a version nobody can name is a version nobody can offer.
// Also switch-complete, so a new command needs a name as well as a verdict.
constexpr const char* commandLabel(daw::UiCommandType type) {
  switch (type) {
    case daw::UiCommandType::None: return "None";
    case daw::UiCommandType::LoadPluginOnTrack: return "Load plugin on track";
    case daw::UiCommandType::WriteNote: return "Write note";
    case daw::UiCommandType::TogglePlay: return "Toggle play";
    case daw::UiCommandType::DeleteNote: return "Delete note";
    case daw::UiCommandType::Undo: return "Undo";
    case daw::UiCommandType::WriteHarmony: return "Write harmony";
    case daw::UiCommandType::DeleteHarmony: return "Delete harmony";
    case daw::UiCommandType::WriteChord: return "Write chord";
    case daw::UiCommandType::DeleteChord: return "Delete chord";
    case daw::UiCommandType::SetTrackHarmonyQuantize: return "Set track harmony quantize";
    case daw::UiCommandType::Redo: return "Redo";
    case daw::UiCommandType::SetLoopRange: return "Set loop range";
    case daw::UiCommandType::SetAutomationTarget: return "Set automation target";
    case daw::UiCommandType::AddDevice: return "Add device";
    case daw::UiCommandType::RemoveDevice: return "Remove device";
    case daw::UiCommandType::MoveDevice: return "Move device";
    case daw::UiCommandType::UpdateDevice: return "Update device";
    case daw::UiCommandType::SetDeviceEuclideanConfig: return "Set device euclidean config";
    case daw::UiCommandType::SetTrackRouting: return "Set track routing";
    case daw::UiCommandType::AddModLink: return "Add mod link";
    case daw::UiCommandType::RemoveModLink: return "Remove mod link";
    case daw::UiCommandType::SetModLinkUid16: return "Set mod link uid16";
    case daw::UiCommandType::SetModSourceValue: return "Set mod source value";
    case daw::UiCommandType::OpenPluginEditor: return "Open plugin editor";
    case daw::UiCommandType::AddPatcherNode: return "Add patcher node";
    case daw::UiCommandType::RemovePatcherNode: return "Remove patcher node";
    case daw::UiCommandType::ConnectPatcherNodes: return "Connect patcher nodes";
    case daw::UiCommandType::SetPatcherNodeConfig: return "Set patcher node config";
    case daw::UiCommandType::SavePatcherPreset: return "Save patcher preset";
    case daw::UiCommandType::RequestClipWindow: return "Request clip window";
    case daw::UiCommandType::SaveProject: return "Save project";
    case daw::UiCommandType::LoadProject: return "Load project";
    case daw::UiCommandType::SetTrackMixer: return "Set track mixer";
    case daw::UiCommandType::Stop: return "Stop";
    case daw::UiCommandType::SetPosition: return "Set position";
    case daw::UiCommandType::SetTrackName: return "Set track name";
    case daw::UiCommandType::RequestChainSnapshot: return "Request chain snapshot";
    case daw::UiCommandType::RequestDeviceParams: return "Request device params";
    case daw::UiCommandType::SetTempo: return "Set tempo";
    case daw::UiCommandType::Quit: return "Quit";
    case daw::UiCommandType::SetDeviceParam: return "Set device param";
    case daw::UiCommandType::RequestWaveform: return "Request waveform";
    case daw::UiCommandType::PreviewNote: return "Preview note";
    case daw::UiCommandType::AddTrack: return "Add track";
    case daw::UiCommandType::RemoveTrack: return "Remove track";
    case daw::UiCommandType::MovePlacement: return "Move placement";
    case daw::UiCommandType::RemovePlacement: return "Remove placement";
    case daw::UiCommandType::ResizePlacement: return "Resize placement";
    case daw::UiCommandType::AddPlacement: return "Add placement";
    case daw::UiCommandType::Panic: return "Panic";
    case daw::UiCommandType::SetLaneQuantize: return "Set lane quantize";
    case daw::UiCommandType::RevertPlacementOverrides: return "Revert placement overrides";
    case daw::UiCommandType::WriteAutomationPoint: return "Write automation point";
    case daw::UiCommandType::SetPlacementEditScope: return "Set placement edit scope";
    case daw::UiCommandType::RequestAutomationLane: return "Request automation lane";
    case daw::UiCommandType::SetModLinkDepth: return "Set mod link depth";
    case daw::UiCommandType::AddMarker: return "Add marker";
    case daw::UiCommandType::RemoveMarker: return "Remove marker";
    case daw::UiCommandType::RenameMarker: return "Rename marker";
    case daw::UiCommandType::MoveMarker: return "Move marker";
    case daw::UiCommandType::SetTimeSignature: return "Set time signature";
    case daw::UiCommandType::InsertRemoveTime: return "Insert remove time";
    case daw::UiCommandType::ForkPlacementClip: return "Fork placement clip";
    case daw::UiCommandType::SwapPlacementClip: return "Swap placement clip";
    case daw::UiCommandType::ClearPlacementAlternate: return "Clear placement alternate";
    case daw::UiCommandType::SamplerLoad: return "Sampler load";
    case daw::UiCommandType::SamplerSetSlot: return "Sampler set slot";
    case daw::UiCommandType::RequestSamplerKit: return "Request sampler kit";
    case daw::UiCommandType::SamplerSlice: return "Sampler slice";
    case daw::UiCommandType::SamplerMarker: return "Sampler marker";
    case daw::UiCommandType::SamplerEmitRows: return "Sampler emit rows";
    case daw::UiCommandType::SaveModule: return "Save module";
    case daw::UiCommandType::LoadModule: return "Load module";
    case daw::UiCommandType::SetRowOps: return "Set row ops";
    case daw::UiCommandType::SamplerSetEnvelope: return "Sampler set envelope";
    case daw::UiCommandType::BulkChunk: return "Bulk chunk";
    case daw::UiCommandType::SamplerSetEnvelopePoints: return "Sampler set envelope points";
    case daw::UiCommandType::SamplerSetLfo: return "Sampler set lfo";
    case daw::UiCommandType::SamplerSetFilter: return "Sampler set filter";
    case daw::UiCommandType::SetTrackSoundAddressed: return "Set track sound addressed";
    case daw::UiCommandType::SamplerSetDevice: return "Sampler set device";
    case daw::UiCommandType::SetTrackCollapsed: return "Set track collapsed";
    case daw::UiCommandType::SamplerSetSlotName: return "Sampler set slot name";
    case daw::UiCommandType::SamplerSetVintage: return "Sampler set vintage";
    case daw::UiCommandType::SetTrackLinesPerBeat: return "Set track lines per beat";
    case daw::UiCommandType::SetTrackAllowNoteOverlap: return "Set track allow note overlap";
    case daw::UiCommandType::SetClipGrid: return "Set clip grid";
    case daw::UiCommandType::SetAudioClipField: return "Set audio clip field";
    case daw::UiCommandType::DeleteAutomationPoint: return "Delete automation point";
    case daw::UiCommandType::RequestSamplerEnvelope: return "Request sampler envelope";
    case daw::UiCommandType::SetClipText: return "Set clip text";
    case daw::UiCommandType::SetMarkerColor: return "Set marker color";
  }
  return "Edit";
}

// One derived answer, so "does it change the document" and "does it open a step" can never
// disagree: a command that changes nothing cannot be Amend, and everything else is a step unless
// it is on the audition list.
constexpr UndoPolicy commandUndoPolicy(daw::UiCommandType type) {
  if (!commandMutatesDocument(type)) {
    return UndoPolicy::None;
  }
  // SwapPlacementClip alone. Fork and ClearPlacementAlternate create and destroy drafts, which is
  // work worth reversing; the swap only chooses which existing clip sounds.
  if (type == daw::UiCommandType::SwapPlacementClip) {
    return UndoPolicy::Amend;
  }
  return UndoPolicy::Version;
}

}  // namespace daw::engine
