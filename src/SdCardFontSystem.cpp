#include "SdCardFontSystem.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include "CrossPointSettings.h"

static uint8_t fontSizeEnumFromSettings() {
  uint8_t e = SETTINGS.fontSize;
  if (e >= CrossPointSettings::FONT_SIZE_COUNT) e = 1;  // default to MEDIUM
  return e;
}

// INF so release builds show what the selected SD font keeps resident (its
// interval and kern tables live in the heap for as long as it is loaded).
bool SdCardFontSystem::loadFamilyLogged(const SdCardFontFamilyInfo& family, GfxRenderer& renderer) {
  const uint32_t before = ESP.getFreeHeap();
  const bool ok = manager_.loadFamily(family, renderer, fontSizeEnumFromSettings());
  if (ok) {
    const uint32_t after = ESP.getFreeHeap();
    LOG_INF("SDFS", "Loaded SD font %s %upt: resident %d bytes, free %u", family.name.c_str(),
            manager_.currentPointSize(), static_cast<int>(before) - static_cast<int>(after), after);
  }
  return ok;
}

void SdCardFontSystem::unload(GfxRenderer& renderer) {
  if (manager_.currentFamilyName().empty()) return;
  const uint32_t before = ESP.getFreeHeap();
  manager_.unloadAll(renderer);
  const uint32_t after = ESP.getFreeHeap();
  LOG_INF("SDFS", "Unloaded SD font: freed %d bytes, free %u", static_cast<int>(after) - static_cast<int>(before),
          after);
}

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  registry_.discover();

  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t fontSizeEnum) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, fontSizeEnum);
  };
  SETTINGS.sdFontResolverCtx = this;

  // Only check that the saved SD font still exists. Loading it keeps its tables resident
  // (~59KB for a 2-style Joyo-kanji font), so it is deferred to ReaderActivity's ensureLoaded().
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto* family = registry_.findFamily(SETTINGS.sdFontFamilyName);
    if (family) {
      LOG_DBG("SDFS", "SD card font family on card: %s (loaded on reader entry)", SETTINGS.sdFontFamilyName);
    } else {
      LOG_DBG("SDFS", "SD font family not found on card: %s (clearing)", SETTINGS.sdFontFamilyName);
      SETTINGS.sdFontFamilyName[0] = '\0';
    }
  }

  LOG_DBG("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
  // If the web server (or another task) installed/deleted fonts, re-discover.
  // Track whether we just re-discovered so we can force a reload below even
  // when the wanted family/size still maps to the same point size — the file
  // contents on disk may have changed (e.g. user re-uploaded a new build).
  const bool registryWasDirty = registryDirty_.exchange(false, std::memory_order_acquire);
  if (registryWasDirty) {
    LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
    registry_.discover();
  }

  const char* wantedFamily = SETTINGS.sdFontFamilyName;
  const std::string& currentFamily = manager_.currentFamilyName();
  const uint8_t sizeEnum = fontSizeEnumFromSettings();

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
    }
    return;
  }

  // Reload if family changed OR if the user-selected size maps to a
  // different file than what's currently loaded OR if the registry was
  // just rediscovered (file may have been replaced on disk).
  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    const auto* family = registry_.findFamily(wantedFamily);
    if (!family) {
      LOG_DBG("SDFS", "SD font family disappeared: %s (clearing)", wantedFamily);
      manager_.unloadAll(renderer);
      SETTINGS.sdFontFamilyName[0] = '\0';
      return;
    }
    auto sizes = family->availableSizes();
    uint8_t idx = sizeEnum;
    if (idx >= sizes.size()) idx = sizes.size() - 1;
    uint8_t wantedPt = sizes.empty() ? 0 : sizes[idx];
    if (!registryWasDirty && wantedPt == manager_.currentPointSize()) return;
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u (enum %u)%s", wantedFamily, manager_.currentPointSize(), wantedPt,
            sizeEnum, registryWasDirty ? " [registry dirty]" : "");
  }

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family) {
    if (loadFamilyLogged(*family, renderer)) {
      LOG_DBG("SDFS", "Loaded SD font family: %s", wantedFamily);
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", wantedFamily);
      SETTINGS.sdFontFamilyName[0] = '\0';
    }
  } else {
    LOG_DBG("SDFS", "SD font family not found: %s (clearing)", wantedFamily);
    SETTINGS.sdFontFamilyName[0] = '\0';
  }
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*fontSizeEnum*/) const {
  // The manager loads exactly one size (closest to SETTINGS.fontSize), so the
  // enum is implicit — always return the single loaded font ID for this family.
  // ensureLoaded() must have been called with the current settings before this.
  return manager_.getFontId(familyName);
}
