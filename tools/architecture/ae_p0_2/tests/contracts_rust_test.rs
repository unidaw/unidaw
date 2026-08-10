include!("../generated/contracts.rs");

fn main() {
    // Literal vector values and the exact generated bundle records are supplied
    // by cross-language.test.mjs from the committed golden-vector document.
    let arguments = std::env::args().collect::<Vec<_>>();
    assert_eq!(arguments.len(), 24);
    let empty_schema_id = &arguments[1];
    let empty_document_id = &arguments[2];
    let bundle_id = &arguments[3];
    let bundle_schema_id = &arguments[4];
    let manifest_schema_id = &arguments[5];
    let transfer_schema_id = &arguments[6];
    let trust_anchor_schema_id = &arguments[7];
    let baseline = &arguments[8];
    let canonical_bundle = &arguments[9];
    let empty_schema_preimage_hex = &arguments[22];
    let empty_document_preimage_hex = &arguments[23];

    assert_eq!(EMPTY_SCHEMA_ID, empty_schema_id);
    assert_eq!(EMPTY_SCHEMA_PREIMAGE_HEX, empty_schema_preimage_hex);
    assert_eq!(EMPTY_DOCUMENT_ID, empty_document_id);
    assert_eq!(EMPTY_DOCUMENT_PREIMAGE_HEX, empty_document_preimage_hex);
    assert_eq!(SCHEMA_BUNDLE_ID, bundle_id);
    assert_eq!(SCHEMA_BUNDLE_SCHEMA_ID, bundle_schema_id);
    assert_eq!(SCHEMA_TRUST_ANCHOR_SCHEMA_ID, trust_anchor_schema_id);
    assert_eq!(OWNERSHIP_MANIFEST_SCHEMA_ID, manifest_schema_id);
    assert_eq!(OWNERSHIP_TRANSFER_SCHEMA_ID, transfer_schema_id);
    assert!(lower_hex_64(EMPTY_SCHEMA_ID));
    assert!(!lower_hex_64(&"A".repeat(64)));
    assert!(valid_path(".gitignore"));
    assert!(valid_path("nested/.authority"));
    assert!(!valid_path("../alias"));
    assert!(!valid_path("C:/absolute"));
    assert!(!valid_path("C:drive-relative"));
    assert!(!valid_path("nested//alias"));
    assert!(!valid_path("windows\\alias"));
    assert_eq!(quote("line\n\"\\"), "\"line\\n\\\"\\\\\"");
    assert_eq!(quote("\u{0008}\t\n\u{000c}\r"), "\"\\b\\t\\n\\f\\r\"");

    let mut records = Vec::new();
    for fields in arguments[10..22].chunks_exact(3) {
        records.push(SchemaRecord {
            canonical_sha256: fields[0].clone(),
            path: fields[1].clone(),
            schema_id: fields[2].clone(),
        });
    }
    assert_eq!(records.len(), 4);
    assert!(records.iter().all(SchemaRecord::validate));
    assert_eq!(
        records[0].canonical_writer(),
        format!(
            "{{\"canonical_sha256\":{},\"path\":{},\"schema_id\":{}}}",
            quote(&records[0].canonical_sha256),
            quote(&records[0].path),
            quote(&records[0].schema_id),
        ),
    );
    let mut invalid_record = records[0].clone();
    invalid_record.path = "unknown.schema.json".into();
    assert!(!invalid_record.validate());
    invalid_record = records[0].clone();
    invalid_record.schema_id = "a".repeat(64);
    assert!(!invalid_record.validate());

    let bundle = SchemaBundleIdentity {
        bundle_id: bundle_id.clone(),
        schema_id: bundle_schema_id.clone(),
        schemas: records,
        version: "1".into(),
    };
    assert!(bundle.validate());
    assert_eq!(bundle.canonical_writer(), canonical_bundle.as_str());
    let mut invalid_bundle = bundle.clone();
    invalid_bundle.version = "01".into();
    assert!(!invalid_bundle.validate());
    invalid_bundle = bundle.clone();
    invalid_bundle.schemas[1].path = invalid_bundle.schemas[0].path.clone();
    assert!(!invalid_bundle.validate());

    let hex_a = "a".repeat(64);
    let hex_b = "b".repeat(64);
    let hex_c = "c".repeat(64);
    let hex_d = "d".repeat(64);
    let hex_e = "e".repeat(64);
    let compatibility_a = CompatibilityRecord {
        bundle_id: hex_a.clone(),
        validator_sha256: hex_b.clone(),
    };
    let compatibility_b = CompatibilityRecord {
        bundle_id: hex_b.clone(),
        validator_sha256: hex_c.clone(),
    };
    assert!(compatibility_a.validate());
    assert_eq!(
        compatibility_a.canonical_writer(),
        format!(
            "{{\"bundle_id\":{},\"validator_sha256\":{}}}",
            quote(&hex_a),
            quote(&hex_b),
        ),
    );
    let mut invalid_compatibility = compatibility_a.clone();
    invalid_compatibility.validator_sha256 = "A".repeat(64);
    assert!(!invalid_compatibility.validate());

    let anchor = SchemaTrustAnchor {
        anchor_id: hex_c.clone(),
        anchor_version: "1".into(),
        compatibility: vec![compatibility_a.clone(), compatibility_b.clone()],
        current_bundle_id: bundle_id.clone(),
        schema_id: trust_anchor_schema_id.clone(),
        validator_sha256: hex_d.clone(),
    };
    assert!(anchor.validate());
    assert_eq!(
        anchor.canonical_writer(),
        format!(
            "{{\"anchor_id\":{},\"anchor_version\":\"1\",\"compatibility\":[{},{}],\"current_bundle_id\":{},\"schema_id\":{},\"validator_sha256\":{}}}",
            quote(&hex_c),
            compatibility_a.canonical_writer(),
            compatibility_b.canonical_writer(),
            quote(bundle_id),
            quote(trust_anchor_schema_id),
            quote(&hex_d),
        ),
    );
    let mut invalid_anchor = anchor.clone();
    invalid_anchor.compatibility.swap(0, 1);
    assert!(!invalid_anchor.validate());

    let entry = OwnershipEntry {
        dependency: "lane-0-bootstrap".into(),
        owner: "codex-worker-2".into(),
        path: ".fixture".into(),
        reviewer: "claude-worker-2".into(),
        state: "planned".into(),
        transfer: "lane-0".into(),
    };
    assert!(entry.validate());
    assert_eq!(
        entry.canonical_writer(),
        r#"{"dependency":"lane-0-bootstrap","owner":"codex-worker-2","path":".fixture","reviewer":"claude-worker-2","state":"planned","transfer":"lane-0"}"#,
    );
    let mut invalid_entry = entry.clone();
    invalid_entry.owner = "unlisted-owner".into();
    assert!(!invalid_entry.validate());
    invalid_entry = entry.clone();
    invalid_entry.path = "path/../alias".into();
    assert!(!invalid_entry.validate());

    let ownership = OwnershipManifest {
        baseline: baseline.clone(),
        entries: vec![entry.clone()],
        manifest_id: hex_e.clone(),
        schema_bundle_id: bundle_id.clone(),
        schema_id: manifest_schema_id.clone(),
    };
    assert!(ownership.validate());
    assert_eq!(
        ownership.canonical_writer(),
        format!(
            "{{\"baseline\":{},\"entries\":[{}],\"manifest_id\":{},\"schema_bundle_id\":{},\"schema_id\":{}}}",
            quote(baseline),
            entry.canonical_writer(),
            quote(&hex_e),
            quote(bundle_id),
            quote(manifest_schema_id),
        ),
    );
    let mut invalid_ownership = ownership.clone();
    invalid_ownership.entries.push(entry.clone());
    assert!(!invalid_ownership.validate());

    let transfer = OwnershipTransfer {
        from: "codex-worker-2".into(),
        path: ".fixture".into(),
        reviewer: "claude-worker-2".into(),
        schema_bundle_id: bundle_id.clone(),
        schema_id: transfer_schema_id.clone(),
        status: "accepted".into(),
        to: "backend".into(),
        transfer_id: hex_a.clone(),
    };
    assert!(transfer.validate());
    assert_eq!(
        transfer.canonical_writer(),
        format!(
            "{{\"from\":\"codex-worker-2\",\"path\":\".fixture\",\"reviewer\":\"claude-worker-2\",\"schema_bundle_id\":{},\"schema_id\":{},\"status\":\"accepted\",\"to\":\"backend\",\"transfer_id\":{}}}",
            quote(bundle_id),
            quote(transfer_schema_id),
            quote(&hex_a),
        ),
    );
    let mut invalid_transfer = transfer.clone();
    invalid_transfer.status = "approved".into();
    assert!(!invalid_transfer.validate());
    invalid_transfer = transfer;
    invalid_transfer.from = "unlisted-owner".into();
    assert!(!invalid_transfer.validate());
}
