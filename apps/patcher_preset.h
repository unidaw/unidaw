#pragma once

#include <string>

#include <boost/property_tree/ptree.hpp>

#include "apps/patcher_graph.h"

namespace daw {

// Type-string mappings, shared so the project file and the preset file name
// nodes and edges identically.
const char* patcherNodeTypeToString(PatcherNodeType type);
const char* patcherEdgeKindToString(PatcherPortKind kind);

// Reads a patcher graph's nodes and edges out of `root` (which must carry
// "nodes" and, for schema 2, "edges"). Populates graph.nodes/edges but does NOT
// call buildPatcherGraph — the caller does, so a project load builds once. Used
// by both the standalone preset loader and the in-project patcher section.
bool readPatcherGraphTree(const boost::property_tree::ptree& root,
                          uint32_t schemaVersion,
                          PatcherGraph& graph,
                          std::string* error);

bool savePatcherPreset(const PatcherGraph& graph,
                       const std::string& path,
                       std::string* error = nullptr);

bool savePatcherPreset(PatcherGraphState& state,
                       const std::string& path,
                       std::string* error = nullptr);

bool loadPatcherPreset(PatcherGraph& graph,
                       const std::string& path,
                       std::string* error = nullptr);

bool loadPatcherPreset(PatcherGraphState& state,
                       const std::string& path,
                       std::string* error = nullptr);

}  // namespace daw
