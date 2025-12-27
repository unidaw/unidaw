#include "apps/juce_scan_worker.h"

int main(int argc, char** argv) {
  const auto config = daw::parseScanArgs(argc, argv);
  return daw::runScan(config);
}
