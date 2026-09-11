#include "glossaray_network.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>

#include <esp_heap_caps.h>
#include <time.h>

namespace {

// The server this board talks to. `./flash` requires yours and bakes it in; it
// is never committed. A build made WITHOUT `./flash`, by running pio directly,
// has no server, and the setup portal's field starts empty so the person
// holding the board chooses one. It deliberately does not fall back to the
// project's own deployment: a board should never quietly spend somebody else's
// inference budget.
#ifdef GLOSSARAY_PROVISIONED_API_URL
constexpr char kDefaultApiUrl[] = GLOSSARAY_PROVISIONED_API_URL;
#else
constexpr char kDefaultApiUrl[] = "";
#endif
constexpr char kBritishTimezone[] = "GMT0BST,M3.5.0/1,M10.5.0/2";
constexpr char kPortalHead[] = R"HTML(
<style>
:root{color-scheme:dark}body{background:#050207!important;color:#c6a0ff!important;font-family:ui-monospace,SFMono-Regular,Menlo,monospace!important}
.wrap,form{max-width:430px!important}h1,h2,h3{color:#c6a0ff!important;letter-spacing:.08em}a{color:#4fdde5!important}
input,select{background:#241033!important;color:#fff!important;border:2px solid #713fad!important;border-radius:2px!important;min-height:46px!important}
button,.btn,input[type=submit]{background:#713fad!important;color:#fff!important;border:0!important;border-radius:2px!important;min-height:48px!important;font-weight:700!important}
.glossaray-intro{border:2px solid #713fad;background:#14081d;padding:18px;margin:14px 0 22px}.glossaray-mark{color:#4fdde5;font-size:1.15rem;font-weight:800;letter-spacing:.12em}.glossaray-intro p{line-height:1.45}.glossaray-hotspot{border-left:4px solid #4fdde5;padding-left:12px;color:#c6a0ff}.glossaray-step{color:#fff;font-weight:700}
</style>
)HTML";
constexpr char kPortalIntro[] = R"HTML(
<div class="glossaray-intro">
 <div class="glossaray-mark">&gt; GLOSSARAY SETUP</div>
 <p><span class="glossaray-step">1.</span> Choose your Wi-Fi network.<br><span class="glossaray-step">2.</span> Enter its password and save.</p>
 <div class="glossaray-hotspot"><strong>ONE-PHONE HOTSPOT</strong><br>Your iPhone cannot share Personal Hotspot while joined to GLOSSARAY-SETUP. Save the hotspot name and password here, then leave this page, turn Personal Hotspot back on and press RESET on Glossaray.</div>
 <p>Home Wi-Fi or a second phone connects directly.</p>
</div>
)HTML";
#ifdef GLOSSARAY_PROVISIONED_DEVICE_TOKEN
constexpr char kProvisionedDeviceToken[] = GLOSSARAY_PROVISIONED_DEVICE_TOKEN;
#else
constexpr char kProvisionedDeviceToken[] = "";
#endif
#ifdef GLOSSARAY_PROVISIONING_REVISION
constexpr char kProvisioningRevision[] = GLOSSARAY_PROVISIONING_REVISION;
#else
constexpr char kProvisioningRevision[] = "";
#endif

// ISRG Root X1, downloaded from letsencrypt.org/certs/isrgrootx1.pem.
// SHA-256 96:BC:EC:06:26:49:76:F3:74:60:77:9A:CF:28:C5:A7:
//        CF:E8:A3:C0:AA:E1:1A:8F:FC:EE:05:C0:BD:DF:08:C6
constexpr char kIsrgRootX1[] = R"PEM(-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)PEM";

Preferences preferences;
char apiUrl[160] = {};
char deviceToken[65] = {};
bool shouldSave = false;
const char* summary = "not run";
constexpr size_t kBblHeaderBytes = 16;
constexpr size_t kMaxTranslationTextBytes = 4096;
constexpr size_t kMaxTranslationWavBytes = 400000;
constexpr size_t kMaxTranslationResponseBytes =
    kBblHeaderBytes + kMaxTranslationTextBytes + kMaxTranslationWavBytes;

void markForSave() {
  shouldSave = true;
}

bool synchronizeClock() {
  configTzTime(kBritishTimezone, "pool.ntp.org", "time.cloudflare.com");
  const unsigned long started = millis();
  while (time(nullptr) < 1704067200 && millis() - started < 15000) {
    delay(100);
  }
  return time(nullptr) >= 1704067200;
}


bool deriveApiUrl(const char* route, String* derived) {
  *derived = apiUrl;
  const int scheme = derived->indexOf("://");
  if (scheme < 0) return false;
  const int path = derived->indexOf('/', scheme + 3);
  if (path >= 0) derived->remove(path);
  *derived += route;
  return true;
}

bool readExactResponse(HTTPClient& request, uint8_t* response,
                       size_t responseLength, unsigned long timeoutMs) {
  WiFiClient* stream = request.getStreamPtr();
  size_t received = 0;
  const unsigned long started = millis();
  while (received < responseLength && millis() - started < timeoutMs) {
    const int available = stream->available();
    if (available <= 0) {
      if (!request.connected()) break;
      delay(2);
      continue;
    }
    const size_t chunk =
        min(static_cast<size_t>(available), responseLength - received);
    const int bytes = stream->read(response + received, chunk);
    if (bytes > 0) received += bytes;
  }
  return received == responseLength;
}

}  // namespace

namespace GlossarayNetwork {

ConfigResult configure(bool forcePortal, PortalStartedCallback onPortalStarted) {
  shouldSave = false;
  preferences.begin("glossaray", false);
  preferences.getString("apiUrl", apiUrl, sizeof(apiUrl));
  preferences.getString("token", deviceToken, sizeof(deviceToken));
  char appliedRevision[65] = {};
  preferences.getString("provisionRev", appliedRevision, sizeof(appliedRevision));
  if (kProvisioningRevision[0] != '\0' &&
      strcmp(appliedRevision, kProvisioningRevision) != 0) {
    if (kDefaultApiUrl[0] != '\0') {
      strlcpy(apiUrl, kDefaultApiUrl, sizeof(apiUrl));
      preferences.putString("apiUrl", apiUrl);
    }
    if (kProvisionedDeviceToken[0] != '\0') {
      strlcpy(deviceToken, kProvisionedDeviceToken, sizeof(deviceToken));
      preferences.putString("token", deviceToken);
    }
    preferences.putString("provisionRev", kProvisioningRevision);
    Serial.println("Network: applied host provisioning from this flash.");
  }
  if (apiUrl[0] == '\0') {
    strlcpy(apiUrl, kDefaultApiUrl, sizeof(apiUrl));
  }
  if (deviceToken[0] == '\0' && kProvisionedDeviceToken[0] != '\0') {
    strlcpy(deviceToken, kProvisionedDeviceToken, sizeof(deviceToken));
    preferences.putString("token", deviceToken);
    Serial.println("Network: seeded host-provisioned device token into NVS.");
  }

  WiFiManager manager;
  manager.setTitle("GLOSSARAY SETUP");
  manager.setClass("invert");
  manager.setCustomHeadElement(kPortalHead);
  // The ESP32 default fast scan can return an empty first portal page while
  // its AP is also active. Scan every channel and retain weak travel networks;
  // WiFiManager's Refresh action remains available as a recovery control.
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  manager.setMinimumSignalQuality(0);
  manager.setConnectTimeout(20);
  manager.setConfigPortalTimeout(180);
  // An iPhone cannot host Personal Hotspot while joined to GLOSSARAY-SETUP. Save
  // submitted WiFi credentials even when immediate validation must fail, then
  // hand control back so the user can resume the hotspot and reset Glossaray.
  manager.setBreakAfterConfig(true);
  manager.setSaveConfigCallback(markForSave);
  manager.setAPCallback([onPortalStarted](WiFiManager*) {
    if (onPortalStarted != nullptr) onPortalStarted();
  });

  WiFiManagerParameter apiUrlParameter(
      "apiUrl", "Glossaray server URL", apiUrl,
      sizeof(apiUrl) - 1, "type='url' maxlength='159'");
  WiFiManagerParameter tokenParameter(
      "token", "Glossaray device token", deviceToken, sizeof(deviceToken) - 1,
      "type='password' maxlength='64'");
  WiFiManagerParameter portalIntro(kPortalIntro);
  manager.addParameter(&portalIntro);
  manager.addParameter(&apiUrlParameter);
  const bool needsToken = deviceToken[0] == '\0';
  if (needsToken) {
    manager.addParameter(&tokenParameter);
  }

  Serial.println("Network: connect saved WiFi or join GLOSSARAY-SETUP.");
  const bool connected = forcePortal
                             ? manager.startConfigPortal("GLOSSARAY-SETUP")
                             : manager.autoConnect("GLOSSARAY-SETUP");
  if (!connected && !shouldSave) {
    preferences.end();
    summary = "WiFi setup timed out";
    return ConfigResult::Failed;
  }

  if (shouldSave) {
    strlcpy(apiUrl, apiUrlParameter.getValue(), sizeof(apiUrl));
    if (needsToken) {
      strlcpy(deviceToken, tokenParameter.getValue(), sizeof(deviceToken));
    }
    preferences.putString("apiUrl", apiUrl);
    preferences.putString("token", deviceToken);
  }
  preferences.end();

  if (!connected) {
    summary = "WiFi credentials saved; hotspot handoff requires reset";
    Serial.println("Network: credentials saved; start hotspot and reset Glossaray.");
    return ConfigResult::HotspotHandoff;
  }

  Serial.printf("Network: connected, IP=%s; token=%s.\n",
                WiFi.localIP().toString().c_str(),
                deviceToken[0] == '\0' ? "missing" : "stored");
  Serial.printf("Network: server=%s.\n", apiUrl);
  summary = deviceToken[0] == '\0' ? "WiFi connected; device token missing"
                                   : "WiFi connected; ready for health";
  return deviceToken[0] != '\0' ? ConfigResult::Ready : ConfigResult::Failed;
}

bool checkHealth() {
  if (WiFi.status() != WL_CONNECTED || deviceToken[0] == '\0') {
    return false;
  }
  if (!synchronizeClock()) {
    summary = "NTP synchronization failed";
    Serial.println("Network: cannot verify TLS before clock synchronization.");
    return false;
  }

  WiFiClientSecure client;
  client.setCACert(kIsrgRootX1);
  HTTPClient request;
  request.setConnectTimeout(10000);
  request.setTimeout(10000);
  String healthApiUrl;
  if (!deriveApiUrl("/healthz", &healthApiUrl) || !request.begin(client, healthApiUrl)) {
    summary = "HTTPS initialization failed";
    return false;
  }
  const int status = request.GET();
  const String body = status > 0 ? request.getString() : "";
  request.end();

  JsonDocument document;
  const bool valid = deserializeJson(document, body) == DeserializationError::Ok;
  const bool healthy = status == 200 && valid && document["ok"] == true;
  Serial.printf("Network: Glossaray API health HTTP %d, service=%s.\n", status,
                healthy ? "ready" : "unavailable");
  summary = healthy ? "Glossaray API health passed" : "Glossaray API health failed";
  return healthy;
}

const char* apiOrigin() { return apiUrl; }
const char* deviceCredential() { return deviceToken; }
const char* tlsRoot() { return kIsrgRootX1; }

void freeTranslation(TranslationResult* result) {
  if (result == nullptr) return;
  heap_caps_free(result->text);
  heap_caps_free(result->wav);
  *result = {};
}

const char* diagnosticSummary() {
  return summary;
}

}  // namespace GlossarayNetwork
