#include "../generated/contracts.hpp"

#include <cassert>
#include <string>
#include <utility>
#include <vector>

using namespace daw::ae_p0_2;

int main(int argc, char** argv) {
    // Literal vector values and the exact generated bundle records are supplied
    // by cross-language.test.mjs from the committed golden-vector document.
    assert(argc == 24);
    const std::string emptySchemaId = argv[1];
    const std::string emptyDocumentId = argv[2];
    const std::string bundleId = argv[3];
    const std::string bundleSchemaId = argv[4];
    const std::string manifestSchemaId = argv[5];
    const std::string transferSchemaId = argv[6];
    const std::string trustAnchorSchemaId = argv[7];
    const std::string baseline = argv[8];
    const std::string canonicalBundle = argv[9];
    const std::string emptySchemaPreimageHex = argv[22];
    const std::string emptyDocumentPreimageHex = argv[23];

    assert(kEmptySchemaId == emptySchemaId);
    assert(kEmptySchemaPreimageHex == emptySchemaPreimageHex);
    assert(kEmptyDocumentId == emptyDocumentId);
    assert(kEmptyDocumentPreimageHex == emptyDocumentPreimageHex);
    assert(kSchemaBundleId == bundleId);
    assert(kSchemaBundleSchemaId == bundleSchemaId);
    assert(kSchemaTrustAnchorSchemaId == trustAnchorSchemaId);
    assert(kOwnershipManifestSchemaId == manifestSchemaId);
    assert(kOwnershipTransferSchemaId == transferSchemaId);
    assert(lowerHex64(kEmptySchemaId));
    assert(!lowerHex64(std::string(64, 'A')));
    assert(validPath(".gitignore"));
    assert(validPath("nested/.authority"));
    assert(!validPath("../alias"));
    assert(!validPath("C:/absolute"));
    assert(!validPath("C:drive-relative"));
    assert(!validPath("nested//alias"));
    assert(!validPath("windows\\alias"));
    assert(quote("line\n\"\\") == "\"line\\n\\\"\\\\\"");
    assert(quote(std::string{"\b\t\n\f\r", 5}) == "\"\\b\\t\\n\\f\\r\"");

    std::vector<SchemaRecord> records;
    for (int index = 10; index < 22; index += 3) {
        records.push_back(SchemaRecord{argv[index], argv[index + 1], argv[index + 2]});
    }
    assert(records.size() == 4);
    for (const auto& record : records) assert(validate(record));
    assert(canonicalWriter(records.front()) ==
        "{\"canonical_sha256\":" + quote(records.front().canonical_sha256) +
        ",\"path\":" + quote(records.front().path) +
        ",\"schema_id\":" + quote(records.front().schema_id) + "}");
    auto invalidRecord = records.front();
    invalidRecord.path = "unknown.schema.json";
    assert(!validate(invalidRecord));
    invalidRecord = records.front();
    invalidRecord.schema_id = std::string(64, 'a');
    assert(!validate(invalidRecord));

    SchemaBundleIdentity bundle{bundleId, bundleSchemaId, records, "1"};
    assert(validate(bundle));
    assert(canonicalWriter(bundle) == canonicalBundle);
    auto invalidBundle = bundle;
    invalidBundle.version = "01";
    assert(!validate(invalidBundle));
    invalidBundle = bundle;
    invalidBundle.schemas[1].path = invalidBundle.schemas[0].path;
    assert(!validate(invalidBundle));

    const std::string hexA(64, 'a');
    const std::string hexB(64, 'b');
    const std::string hexC(64, 'c');
    const std::string hexD(64, 'd');
    const std::string hexE(64, 'e');
    CompatibilityRecord compatibilityA{hexA, hexB};
    CompatibilityRecord compatibilityB{hexB, hexC};
    assert(validate(compatibilityA));
    assert(canonicalWriter(compatibilityA) ==
        "{\"bundle_id\":" + quote(hexA) + ",\"validator_sha256\":" + quote(hexB) + "}");
    auto invalidCompatibility = compatibilityA;
    invalidCompatibility.validator_sha256 = std::string(64, 'A');
    assert(!validate(invalidCompatibility));

    SchemaTrustAnchor anchor{
        hexC,
        "1",
        {compatibilityA, compatibilityB},
        bundleId,
        trustAnchorSchemaId,
        hexD,
    };
    assert(validate(anchor));
    assert(canonicalWriter(anchor) ==
        "{\"anchor_id\":" + quote(hexC) +
        ",\"anchor_version\":\"1\",\"compatibility\":[" + canonicalWriter(compatibilityA) +
        "," + canonicalWriter(compatibilityB) + "],\"current_bundle_id\":" + quote(bundleId) +
        ",\"schema_id\":" + quote(trustAnchorSchemaId) +
        ",\"validator_sha256\":" + quote(hexD) + "}");
    auto invalidAnchor = anchor;
    std::swap(invalidAnchor.compatibility[0], invalidAnchor.compatibility[1]);
    assert(!validate(invalidAnchor));

    OwnershipEntry entry{
        "lane-0-bootstrap",
        "codex-worker-2",
        ".fixture",
        "claude-worker-2",
        "planned",
        "lane-0",
    };
    assert(validate(entry));
    assert(canonicalWriter(entry) ==
        "{\"dependency\":\"lane-0-bootstrap\",\"owner\":\"codex-worker-2\","
        "\"path\":\".fixture\",\"reviewer\":\"claude-worker-2\","
        "\"state\":\"planned\",\"transfer\":\"lane-0\"}");
    auto invalidEntry = entry;
    invalidEntry.owner = "unlisted-owner";
    assert(!validate(invalidEntry));
    invalidEntry = entry;
    invalidEntry.path = "path/../alias";
    assert(!validate(invalidEntry));

    OwnershipManifest ownership{
        baseline,
        {entry},
        hexE,
        bundleId,
        manifestSchemaId,
    };
    assert(validate(ownership));
    assert(canonicalWriter(ownership) ==
        "{\"baseline\":" + quote(baseline) + ",\"entries\":[" + canonicalWriter(entry) +
        "],\"manifest_id\":" + quote(hexE) + ",\"schema_bundle_id\":" + quote(bundleId) +
        ",\"schema_id\":" + quote(manifestSchemaId) + "}");
    auto invalidOwnership = ownership;
    invalidOwnership.entries.push_back(entry);
    assert(!validate(invalidOwnership));

    OwnershipTransfer transfer{
        "codex-worker-2",
        ".fixture",
        "claude-worker-2",
        bundleId,
        transferSchemaId,
        "accepted",
        "backend",
        hexA,
    };
    assert(validate(transfer));
    assert(canonicalWriter(transfer) ==
        "{\"from\":\"codex-worker-2\",\"path\":\".fixture\","
        "\"reviewer\":\"claude-worker-2\",\"schema_bundle_id\":" + quote(bundleId) +
        ",\"schema_id\":" + quote(transferSchemaId) +
        ",\"status\":\"accepted\",\"to\":\"backend\",\"transfer_id\":" + quote(hexA) + "}");
    auto invalidTransfer = transfer;
    invalidTransfer.status = "approved";
    assert(!validate(invalidTransfer));
    invalidTransfer = transfer;
    invalidTransfer.from = "unlisted-owner";
    assert(!validate(invalidTransfer));

    return 0;
}
