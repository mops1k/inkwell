#include "OtaUpdater.h"

#include <Arduino.h>
#include <FirmwareManifestParser.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <ReleaseJsonParser.h>

#include <cctype>
#include <cstdio>
#include <cstring>

#include "BootHealth.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip's
// ip4_addr.h unless seen before the ESP-IDF headers below (esp_wifi.h and
// friends transitively include lwip). Pin this order; clang-format would
// otherwise sort the local headers last and break the build.
#include "CrossPointSettings.h"
#include "HttpDownloader.h"
#include <bootloader_common.h>
#include <esp_flash_partitions.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <wolfssl/wolfcrypt/sha256.h>
// clang-format on

namespace {
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/" CROSSPOINT_GIT_REPOSITORY "/releases/latest";
constexpr char releaseListUrl[] = "https://api.github.com/repos/" CROSSPOINT_GIT_REPOSITORY "/releases?per_page=1";
constexpr int otaHttpMaxAttempts = 3;
constexpr unsigned long otaInitialRetryDelayMs = 1000;
constexpr size_t releaseMetadataMaxBytes = 128 * 1024;
constexpr size_t sha256HexLength = 64;
// A wolfSSL client handshake on this board needs a contiguous block in the tens
// of kilobytes. Checking it up front turns an out-of-memory abort() inside the
// TLS stack (an exception-free build, so it reboots) into a reported error.
constexpr size_t tlsMinLargestBlockBytes = 40 * 1024;

const char* getReleaseApiUrl() { return SETTINGS.includeBetaUpdates ? releaseListUrl : latestReleaseUrl; }

bool haveHeapForTls(const char* operation) {
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  const size_t freeBytes = esp_get_free_heap_size();
  LOG_DBG("OTA", "%s: free heap %zu B, largest block %zu B", operation, freeBytes, largest);
  if (largest < tlsMinLargestBlockBytes) {
    LOG_ERR("OTA", "%s skipped: largest free block %zu B < %zu B", operation, largest, tlsMinLargestBlockBytes);
    return false;
  }
  return true;
}

// The OTA session runs on the Arduino loop task, whose stack also carried the
// WiFi association just before. Logging the high-water mark (minimum free stack
// ever seen by this task) makes a future overflow visible in the crash report
// instead of only after the fact.
void logStackMargin(const char* stage) {
  LOG_INF("OTA", "%s: stack high-water %u B free", stage, static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

// The manifest names assets by file name only; the release metadata carries the
// full download URL. Comparing the URL's last path segment keeps both sides
// honest: the verified hash must describe the file that is actually streamed.
const char* pathBasename(const char* path) {
  if (!path) return "";
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

bool assetNameMatches(const char* url, const char* fileName) {
  if (!fileName || *fileName == '\0') return false;
  return strcmp(pathBasename(url), fileName) == 0;
}

// Defence in depth behind the manifest hash: the image in the update slot must
// identify itself as an Inkwell build and carry the version the release claims.
// The Arduino-ESP32 builds this project ships with an empty app descriptor
// (verified on the 1.0.0 image), so an empty field is treated as "legacy, not
// verifiable here" instead of failing every install — the SHA-256 from the
// signed-release manifest is the binding check. Once the descriptor is
// populated at build time these comparisons tighten on their own.
bool imageDescriptionMatches(const esp_partition_t* partition, const std::string& expectedVersion) {
  esp_app_desc_t description{};
  const esp_err_t err = esp_ota_get_partition_description(partition, &description);
  if (err != ESP_OK) {
    LOG_ERR("OTA", "cannot read image description: %s", esp_err_to_name(err));
    return false;
  }

  const bool hasProject = description.project_name[0] != '\0';
  const bool hasVersion = description.version[0] != '\0';
  LOG_INF("OTA", "image in update slot: project=%s version=%s", hasProject ? description.project_name : "<none>",
          hasVersion ? description.version : "<none>");

  if (hasProject && strcasecmp(description.project_name, "Inkwell") != 0) {
    LOG_ERR("OTA", "image belongs to another project: %s", description.project_name);
    return false;
  }
  if (hasVersion && expectedVersion != description.version) {
    LOG_ERR("OTA", "image version %s does not match release %s", description.version, expectedVersion.c_str());
    return false;
  }
  return true;
}

void delayBeforeRetry(const char* operation, int attempt) {
  const unsigned long delayMs = otaInitialRetryDelayMs << static_cast<unsigned int>(attempt - 1);
  LOG_ERR("OTA", "%s failed on attempt %d/%d, retrying in %lu ms", operation, attempt, otaHttpMaxAttempts, delayMs);
  delay(delayMs);
}

struct WifiPowerSaveGuard {
  WifiPowerSaveGuard() { esp_wifi_set_ps(WIFI_PS_NONE); }
  ~WifiPowerSaveGuard() { esp_wifi_set_ps(WIFI_PS_MIN_MODEM); }
};

} /* namespace */

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  updateAvailable = false;
  latestVersion.clear();
  otaUrl.clear();
  downloadErrorDetails.clear();
  otaSize = 0;
  processedSize = 0;
  totalSize = 0;
  render = false;

  const char* releaseApiUrl = getReleaseApiUrl();

  // Keep WiFi out of modem-sleep while doing release metadata HTTPS I/O.
  // This mirrors installUpdate() and reduces intermittent TLS read stalls.
  WifiPowerSaveGuard wifiPowerSaveGuard;

  // Use one streaming parser path for both stable and beta update checks to
  // avoid holding full GitHub release JSON in memory.
  // Adapted from crosspoint-reader/crosspoint-reader (MIT),
  // PR #1810 by znelson and contributors.
  if (!haveHeapForTls("Release metadata")) {
    return LOW_MEMORY;
  }
  LOG_DBG("OTA", "Checking for %s update at %s", SETTINGS.includeBetaUpdates ? "beta" : "stable", releaseApiUrl);
  for (int attempt = 1; attempt <= otaHttpMaxAttempts; ++attempt) {
    ReleaseJsonParser releaseParser;
    size_t bytesSeen = 0;
    const bool ok = HttpDownloader::fetchUrl(
        releaseApiUrl,
        [&releaseParser, &bytesSeen](const uint8_t* data, size_t len) {
          bytesSeen += len;
          if (bytesSeen > releaseMetadataMaxBytes) {
            return false;
          }
          releaseParser.feed(reinterpret_cast<const char*>(data), len);
          // For OTA metadata we need tag_name, the firmware asset and whatever
          // verifies it — the asset digest, or the manifest asset to fetch next.
          // Stop early once those are known to avoid fragile tail reads.
          if (releaseParser.foundTag() && releaseParser.foundFirmware() &&
              (releaseParser.foundDigest() || releaseParser.foundManifest())) {
            return false;
          }
          return true;
        },
        true);

    if (!ok) {
      if (bytesSeen > releaseMetadataMaxBytes) {
        LOG_ERR("OTA", "Release metadata too large after %zu bytes", bytesSeen);
        return METADATA_TOO_LARGE_ERROR;
      }

      LOG_ERR("OTA", "Release metadata stream failed on attempt %d/%d", attempt, otaHttpMaxAttempts);
      if (attempt < otaHttpMaxAttempts) {
        delayBeforeRetry("Release metadata stream", attempt);
        continue;
      }
      return HTTP_ERROR;
    }

    if (!releaseParser.foundTag()) {
      LOG_ERR("OTA", "No tag_name found in release metadata");
      return JSON_PARSE_ERROR;
    }

    if (!releaseParser.foundFirmware()) {
      LOG_ERR("OTA", "No firmware asset in release metadata (inkwell-<version>.bin or firmware.bin)");
      return NO_UPDATE;
    }

    latestVersion = releaseParser.getTagName();
    otaUrl = releaseParser.getFirmwareUrl();
    otaSize = releaseParser.getFirmwareSize();
    totalSize = otaSize;

    // Verification source, in order of preference:
    //   1. the asset digest GitHub reports next to the asset in the same release
    //      metadata — no extra HTTPS request, which is the cheapest option on a
    //      board that already runs tight during TLS;
    //   2. firmware-manifest.json, which additionally names the asset per board.
    // Without either, the release is not installed (fail closed).
    if (releaseParser.foundDigest()) {
      expectedSha256 = releaseParser.getFirmwareDigest();
      expectedFileName.clear();
      LOG_DBG("OTA", "Verifying with the release asset digest (%s)", expectedSha256.c_str());
    } else {
      if (!releaseParser.foundManifest()) {
        LOG_ERR("OTA", "Release has neither an asset digest nor firmware-manifest.json");
        return MANIFEST_MISSING;
      }
      if (!haveHeapForTls("firmware-manifest.json")) {
        return LOW_MEMORY;
      }
      const char* board = gpio.deviceIsX3() ? "X3" : "X4";
      const char* supportedBoards[] = {board};
      FirmwareManifestParser manifestParser(supportedBoards, 1);
      const bool manifestOk = HttpDownloader::fetchUrl(
          releaseParser.getManifestUrl(),
          [&manifestParser](const uint8_t* data, size_t len) {
            manifestParser.feed(reinterpret_cast<const char*>(data), len);
            return true;
          },
          /*verifyTls=*/true);
      if (!manifestOk) {
        LOG_ERR("OTA", "firmware-manifest.json download failed");
        return HTTP_ERROR;
      }
      if (!manifestParser.hasSelected()) {
        LOG_ERR("OTA", "firmware-manifest.json has no application for %s", board);
        return MANIFEST_INVALID;
      }
      expectedSha256 = manifestParser.selectedSha256();
      expectedFileName = manifestParser.selectedFile();
      if (!assetNameMatches(otaUrl.c_str(), expectedFileName.c_str())) {
        LOG_ERR("OTA", "manifest names %s but the release asset is %s", expectedFileName.c_str(),
                pathBasename(otaUrl.c_str()));
        return MANIFEST_INVALID;
      }
      if (otaSize != 0 && manifestParser.selectedBytes() != 0 && otaSize != manifestParser.selectedBytes()) {
        LOG_ERR("OTA", "manifest size %zu does not match asset size %zu", manifestParser.selectedBytes(), otaSize);
        return MANIFEST_INVALID;
      }
    }

    updateAvailable = true;

    logStackMargin("Update check");
    LOG_DBG("OTA", "Found %s update: %s", SETTINGS.includeBetaUpdates ? "beta" : "stable", latestVersion.c_str());
    return OK;
  }

  return HTTP_ERROR;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty() || latestVersion == CROSSPOINT_VERSION) {
    return false;
  }

  int currentMajor = 0, currentMinor = 0, currentPatch = 0, currentBetaRelease = 0, currentBetaBuild = 0;
  int latestMajor = 0, latestMinor = 0, latestPatch = 0, latestBetaRelease = 0, latestBetaBuild = 0;

  const auto currentVersion = CROSSPOINT_VERSION;
  const bool currentIsBeta = strstr(currentVersion, "-rc.") != nullptr;
  const bool latestIsBeta = latestVersion.find("-rc.") != std::string::npos;

  // Semantic version check with optional RC suffix. `sscanf()` will stop when
  // it reaches part of the input string that doesn't match the format, so this
  // format string works for versions like "1.31", "1.34.2", "1.35.0-rc.1", and
  // "1.36.0-rc.2.5".
  // This does not handle versions using the old "rc.<hash>" format, but
  // considering that people will need to manually install this release or later
  // to get this functionality anyway that should be fine.
  sscanf(latestVersion.c_str(), "%d.%d.%d-rc.%d.%d", &latestMajor, &latestMinor, &latestPatch, &latestBetaRelease,
         &latestBetaBuild);
  sscanf(currentVersion, "%d.%d.%d-rc.%d.%d", &currentMajor, &currentMinor, &currentPatch, &currentBetaRelease,
         &currentBetaBuild);

  /*
   * Compare major versions.
   * If they differ, return true if latest major version greater than current major version
   * otherwise return false.
   */
  if (latestMajor != currentMajor) return latestMajor > currentMajor;

  /*
   * Compare minor versions.
   * If they differ, return true if latest minor version greater than current minor version
   * otherwise return false.
   */
  if (latestMinor != currentMinor) return latestMinor > currentMinor;

  /*
   * Check patch versions.
   */
  if (latestPatch != currentPatch) return latestPatch > currentPatch;

  /*
   * If we reach here, the stable version segments are equal. A stable release
   * is newer than an RC with the same version.
   */
  if (!latestIsBeta && currentIsBeta) {
    return true;
  }

  if (latestIsBeta && !currentIsBeta) {
    return false;
  }

  /*
   * If both versions are RCs, compare their RC release and build numbers.
   */
  if (latestIsBeta && currentIsBeta) {
    if (latestBetaRelease != currentBetaRelease) {
      return latestBetaRelease > currentBetaRelease;
    }
    if (latestBetaBuild != currentBetaBuild) {
      return latestBetaBuild > currentBetaBuild;
    }
  }

  return false;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

void OtaUpdater::cleanupUpdate() {
  if (otaWriteHandle) {
    const esp_err_t err = esp_ota_abort(reinterpret_cast<esp_ota_handle_t>(otaWriteHandle));
    if (err != ESP_OK) {
      LOG_ERR("OTA", "esp_ota_abort on cleanup: %s", esp_err_to_name(err));
    }
    otaWriteHandle = nullptr;
  }
  cancelRequested = false;
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

void OtaUpdater::cancelUpdate() {
  // If a streaming install is mid-flight, the DataCallback observes
  // cancelRequested and aborts; the step then cleans up. If not started yet,
  // just set the flag so the next step returns UPDATE_CANCELLED.
  cancelRequested = true;
}

OtaUpdater::OtaUpdaterError OtaUpdater::beginInstallUpdate() {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  cleanupUpdate();
  render = false;
  cancelRequested = false;
  installDone = false;
  installResult = OK;
  processedSize = 0;
  downloadErrorDetails.clear();

  // Open the next OTA partition for streaming writes. The actual firmware
  // download runs in performInstallUpdateStep() via the wolfSSL HttpDownloader,
  // so no mbedtls esp_https_ota is involved.
  const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
  if (updatePartition == nullptr) {
    LOG_ERR("OTA", "no next OTA partition");
    return INTERNAL_UPDATE_ERROR;
  }

  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_ota_handle_t handle = 0;
  const esp_err_t err = esp_ota_begin(updatePartition, OTA_SIZE_UNKNOWN, &handle);
  if (err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_begin failed: %s", esp_err_to_name(err));
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return INTERNAL_UPDATE_ERROR;
  }
  otaWriteHandle = reinterpret_cast<void*>(handle);
  LOG_INF("OTA", "esp_ota_begin OK on %s; streaming from %s", updatePartition->label, otaUrl.c_str());
  return UPDATE_IN_PROGRESS;
}

/* Writes the otadata entry to boot from the most recently flashed OTA partition,
 * bypassing esp_ota_set_boot_partition()'s image_validate() call.
 * Used when esp_ota_end()/esp_ota_set_boot_partition() returns
 * ESP_ERR_OTA_VALIDATE_FAILED on unsigned Arduino builds (boot_comm efuse
 * revision check false-positive). */
int OtaUpdater::forceSetOtaBootPartition() {
  const esp_partition_t* newPartition = esp_ota_get_next_update_partition(nullptr);
  if (newPartition == nullptr) {
    LOG_ERR("OTA", "force boot partition: next update partition not found");
    return ESP_ERR_NOT_FOUND;
  }
  LOG_INF("OTA", "force boot next partition=%s subtype=0x%x offset=0x%lx size=0x%lx", newPartition->label,
          newPartition->subtype, static_cast<unsigned long>(newPartition->address),
          static_cast<unsigned long>(newPartition->size));

  const esp_partition_t* otaDataPartition =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  if (otaDataPartition == nullptr) {
    LOG_ERR("OTA", "force boot partition: otadata partition not found");
    return ESP_ERR_NOT_FOUND;
  }
  esp_ota_select_entry_t otadata[2];
  esp_err_t err = esp_partition_read(otaDataPartition, 0, &otadata[0], sizeof(esp_ota_select_entry_t));
  if (err != ESP_OK) {
    LOG_ERR("OTA", "force boot: read otadata[0] failed: %s", esp_err_to_name(err));
    return err;
  }
  err = esp_partition_read(otaDataPartition, otaDataPartition->erase_size, &otadata[1], sizeof(esp_ota_select_entry_t));
  if (err != ESP_OK) {
    LOG_ERR("OTA", "force boot: read otadata[1] failed: %s", esp_err_to_name(err));
    return err;
  }

  int activeSlot = bootloader_common_get_active_otadata(otadata);
  int nextSlot = (activeSlot == -1) ? 0 : (~activeSlot & 1);

  uint8_t otaAppCount = 0;
  while (esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                  static_cast<esp_partition_subtype_t>(ESP_PARTITION_SUBTYPE_APP_OTA_MIN + otaAppCount),
                                  nullptr) != nullptr) {
    otaAppCount++;
  }
  if (otaAppCount == 0) {
    LOG_ERR("OTA", "force boot select: no OTA app partitions found");
    return ESP_ERR_NOT_FOUND;
  }

  const uint8_t subTypeId = newPartition->subtype & 0x0F;
  uint32_t newSeq;
  if (activeSlot == -1) {
    newSeq = subTypeId + 1;
  } else {
    uint32_t currentSeq = otadata[activeSlot].ota_seq;
    newSeq = currentSeq;
    // ESP-IDF's bootloader maps ota_seq to an OTA app slot with
    // (ota_seq - 1) % ota_app_count. Match that mapping here so the forced
    // otadata entry selects the partition the streaming install just wrote.
    while ((newSeq - 1) % otaAppCount != static_cast<uint32_t>(subTypeId)) {
      newSeq++;
    }
    if (newSeq == currentSeq) newSeq += otaAppCount;
  }

  otadata[nextSlot].ota_seq = newSeq;
  otadata[nextSlot].ota_state = ESP_OTA_IMG_NEW;
  otadata[nextSlot].crc = bootloader_common_ota_select_crc(&otadata[nextSlot]);

  err = esp_partition_erase_range(otaDataPartition, otaDataPartition->erase_size * static_cast<uint32_t>(nextSlot),
                                  otaDataPartition->erase_size);
  if (err != ESP_OK) {
    LOG_ERR("OTA", "force boot: erase otadata[%d] failed: %s", nextSlot, esp_err_to_name(err));
    return err;
  }

  err = esp_partition_write(otaDataPartition, otaDataPartition->erase_size * static_cast<uint32_t>(nextSlot),
                            &otadata[nextSlot], sizeof(esp_ota_select_entry_t));
  if (err != ESP_OK) {
    LOG_ERR("OTA", "force boot: write otadata[%d] failed: %s", nextSlot, esp_err_to_name(err));
  }
  return err;
}

OtaUpdater::OtaUpdaterError OtaUpdater::performInstallUpdateStep() {
  if (cancelRequested) {
    cleanupUpdate();
    return UPDATE_CANCELLED;
  }
  if (!otaWriteHandle) {
    return INTERNAL_UPDATE_ERROR;
  }
  // The whole streaming download happens in this single call; the DataCallback
  // drives progress/cancel via installProgressCb so the UI stays responsive.
  // Idempotent guard: once done, don't re-enter.
  if (installDone) {
    return installResult;
  }

  const esp_ota_handle_t handle = reinterpret_cast<esp_ota_handle_t>(otaWriteHandle);
  bool writeOk = true;
  bool userCancelled = false;
  HttpDownloader::VerifiedFetchStatus downloadStatus;

  if (expectedSha256.size() != sha256HexLength) {
    LOG_ERR("OTA", "No expected SHA-256 from the release manifest");
    cleanupUpdate();
    installResult = MANIFEST_INVALID;
    return installResult;
  }
  wc_Sha256 sha256;
  wc_InitSha256(&sha256);

  // Verify-only (fail closed): a MITM must not be able to downgrade the firmware
  // download to an unverified connection by presenting a bad cert.
  const bool fetchOk = HttpDownloader::fetchUrlVerified(
      otaUrl,
      [&](const uint8_t* data, size_t len) -> bool {
        if (cancelRequested) {
          userCancelled = true;
          return false;
        }
        if (esp_ota_write(handle, data, len) != ESP_OK) {
          writeOk = false;
          return false;  // abort
        }
        wc_Sha256Update(&sha256, data, static_cast<word32>(len));
        processedSize += len;
        render = true;
        // Let the host redraw progress and poll for Back; abort if it returns false.
        if (installProgressCb && !installProgressCb(processedSize, totalSize)) {
          userCancelled = true;
          return false;
        }
        return true;
      },
      /*treatAbortAsSuccess=*/false, "", "", &downloadStatus);

  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  installDone = true;

  if (userCancelled || cancelRequested) {
    LOG_INF("OTA", "Install cancelled by user");
    cleanupUpdate();
    installResult = UPDATE_CANCELLED;
    return installResult;
  }
  if (!writeOk) {
    LOG_ERR("OTA", "esp_ota_write failed");
    cleanupUpdate();
    installResult = INTERNAL_UPDATE_ERROR;
    return installResult;
  }
  if (!fetchOk) {
    char details[84];
    snprintf(details, sizeof(details), "HTTP %d / %d | TLS %d | %zu B", downloadStatus.result,
             downloadStatus.responseStatus, downloadStatus.tlsError, processedSize);
    downloadErrorDetails = details;
    LOG_ERR("OTA", "firmware download failed");
    cleanupUpdate();
    installResult = HTTP_ERROR;
    return installResult;
  }

  // The image is already in the update slot, but the boot partition is not
  // switched yet: a hash mismatch aborts here, so a corrupted or swapped asset
  // never becomes bootable.
  uint8_t digest[WC_SHA256_DIGEST_SIZE];
  if (wc_Sha256Final(&sha256, digest) != 0) {
    LOG_ERR("OTA", "SHA-256 finalization failed");
    cleanupUpdate();
    installResult = INTERNAL_UPDATE_ERROR;
    return installResult;
  }
  char actualSha256[sha256HexLength + 1];
  firmware_manifest::formatSha256Hex(digest, sizeof(digest), actualSha256, sizeof(actualSha256));
  // The digest proves what was written; the length check catches a truncated
  // transfer that still hashed consistently (an upstream proxy closing early).
  if (totalSize != 0 && processedSize != totalSize) {
    LOG_ERR("OTA", "image is %zu B, release asset is %zu B", processedSize, totalSize);
    cleanupUpdate();
    installResult = VALIDATE_FAILED;
    return installResult;
  }
  if (!firmware_manifest::sha256HexEquals(actualSha256, expectedSha256.c_str())) {
    char details[96];
    snprintf(details, sizeof(details), "SHA256 %s != %s", actualSha256, expectedSha256.c_str());
    downloadErrorDetails = details;
    LOG_ERR("OTA", "SHA-256 mismatch: expected %s, got %s", expectedSha256.c_str(), actualSha256);
    cleanupUpdate();
    installResult = VALIDATE_FAILED;
    return installResult;
  }

  esp_err_t finish_err = esp_ota_end(handle);
  otaWriteHandle = nullptr;
  if (finish_err != ESP_OK && finish_err != ESP_ERR_OTA_VALIDATE_FAILED) {
    LOG_ERR("OTA", "esp_ota_end failed: %s", esp_err_to_name(finish_err));
    installResult = INTERNAL_UPDATE_ERROR;
    return installResult;
  }

  // The image is written and structurally validated; before it can become
  // bootable, confirm it identifies itself as this project at this version.
  const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
  if (updatePartition == nullptr) {
    LOG_ERR("OTA", "no next OTA partition after install");
    installResult = INTERNAL_UPDATE_ERROR;
    return installResult;
  }
  if (!imageDescriptionMatches(updatePartition, latestVersion)) {
    installResult = VALIDATE_FAILED;
    return installResult;
  }

  // Set the new partition as boot. Arduino unsigned builds fail image_validate()
  // in esp_ota_set_boot_partition (and esp_ota_end returns VALIDATE_FAILED), so
  // fall back to writing otadata directly — same bypass as before.
  if (!boot_health::stageUpdate(updatePartition)) {
    LOG_ERR("OTA", "Cannot persist update recovery record");
    installResult = INTERNAL_UPDATE_ERROR;
    return installResult;
  }
  esp_err_t boot_err = (finish_err == ESP_ERR_OTA_VALIDATE_FAILED) ? ESP_ERR_OTA_VALIDATE_FAILED
                                                                   : esp_ota_set_boot_partition(updatePartition);
  if (boot_err == ESP_ERR_OTA_VALIDATE_FAILED) {
    LOG_INF("OTA", "Validation failed (expected for unsigned Arduino builds) - forcing boot partition");
    boot_err = static_cast<esp_err_t>(forceSetOtaBootPartition());
    if (boot_err != ESP_OK) {
      LOG_ERR("OTA", "forceSetOtaBootPartition failed: %s", esp_err_to_name(boot_err));
      installResult = VALIDATE_FAILED;
      return installResult;
    }
  } else if (boot_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_set_boot_partition failed: %s", esp_err_to_name(boot_err));
    installResult = INTERNAL_UPDATE_ERROR;
    return installResult;
  }

  LOG_INF("OTA", "Update completed (%zu bytes)", processedSize);
  logStackMargin("Update install");
  installResult = OK;
  return installResult;
}
