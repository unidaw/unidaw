#[allow(dead_code)]
pub const SCHEMA_BUNDLE_ID: &str = "ae-p0-2.schema-bundle-identity";
pub fn validate_manifest(entries: &[&str]) -> bool { let mut x=entries.to_vec(); x.sort(); x.windows(2).all(|w| w[0]!=w[1]) }
