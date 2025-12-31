use std::fs;
use std::path::PathBuf;

use anyhow::{Context as AnyhowContext, Result};
use serde::Deserialize;

#[derive(Clone, Debug)]
pub struct PluginEntry {
    pub index: usize,
    pub name: String,
    pub vendor: String,
    pub is_instrument: bool,
}

#[derive(Deserialize)]
struct PluginCacheFile {
    #[serde(default)]
    plugins: Vec<PluginCacheEntry>,
}

#[derive(Deserialize)]
struct PluginCacheEntry {
    name: String,
    vendor: String,
    #[serde(default)]
    is_instrument: bool,
    #[serde(default)]
    ok: bool,
    #[serde(default)]
    scan_status: String,
    #[serde(default)]
    error: String,
}

fn default_plugin_cache_path() -> PathBuf {
    if let Ok(path) = std::env::var("DAW_PLUGIN_CACHE") {
        return PathBuf::from(path);
    }
    if let Ok(cwd) = std::env::current_dir() {
        for ancestor in cwd.ancestors() {
            let build_candidate = ancestor.join("build/plugin_cache.json");
            if build_candidate.exists() {
                return build_candidate;
            }
            let flat_candidate = ancestor.join("plugin_cache.json");
            if flat_candidate.exists() {
                return flat_candidate;
            }
        }
    }
    PathBuf::from("build/plugin_cache.json")
}

pub fn load_plugin_cache() -> Vec<PluginEntry> {
    let path = default_plugin_cache_path();
    let data = fs::read_to_string(&path);
    let Ok(json) = data else {
        eprintln!(
            "daw-app: plugin cache not found at {}",
            path.display()
        );
        return Vec::new();
    };

    let parsed: Result<PluginCacheFile> = serde_json::from_str(&json)
        .context("failed to parse plugin cache JSON")
        .map_err(|err| err);
    match parsed {
        Ok(cache) => {
            let plugins = cache
                .plugins
                .into_iter()
                .enumerate()
                .filter(|(_, entry)| {
                    entry.ok
                        || entry.scan_status.eq_ignore_ascii_case("ok")
                        || entry.error.is_empty()
                })
                .map(|(index, entry)| PluginEntry {
                    index,
                    name: entry.name,
                    vendor: entry.vendor,
                    is_instrument: entry.is_instrument,
                })
                .collect::<Vec<_>>();
            eprintln!(
                "daw-app: loaded {} plugins from {}",
                plugins.len(),
                path.display()
            );
            plugins
        }
        Err(_err) => Vec::new(),
    }
}
