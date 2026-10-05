#include <OpenHaldexC6_WebAccess.h>
#include <OpenHaldexC6_Access.h>
#include <OpenHaldexC6_Calculations.h>

// ---------------------------------------------------------------------------
// Web access gate. One handler, registered first, that claims every request
// that is not allowed through yet and answers it:
//   - provisioned AP client .......... not claimed (trusted, joined the WPA2 AP)
//   - STA (home network) client ...... 401 + WWW-Authenticate until it sends
//                                      Basic auth admin / AP password
//   - unprovisioned device ........... AP client is sent to /setup, STA refused
// It claims the request before any route sees it, so the check covers the UI
// pages, /api/*, /ota/* and the recovery page alike. An upload that gets
// refused here has its body drained by the base class no-op handlers.
// ---------------------------------------------------------------------------

static const char SETUP_PAGE[] PROGMEM = R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>OpenHaldex Edge - Setup</title><style>
body{font-family:sans-serif;background:#111;color:#eee;display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0}
.card{background:#222;border-radius:8px;padding:2rem;max-width:400px;width:100%}
h1{margin:0 0 .4rem;font-size:1.4rem}p{color:#aaa;font-size:.9rem;margin:0 0 1.5rem}
label{display:block;margin-bottom:.3rem;font-size:.85rem}
input[type=password]{width:100%;box-sizing:border-box;padding:.6rem;border-radius:4px;border:1px solid #444;background:#333;color:#eee;font-size:1rem;margin-bottom:1rem}
button{width:100%;padding:.7rem;background:#c0392b;border:none;border-radius:4px;color:#fff;font-size:1rem;cursor:pointer}
button:disabled{opacity:.5;cursor:default}.msg{font-size:.85rem;margin-bottom:1rem;display:none}.err{color:#ff6b6b}.ok{color:#4caf50}
</style></head><body><div class="card"><h1>OpenHaldex Edge</h1>
<p>Set a WiFi password to secure this device. Minimum 8 characters. The access point restarts protected - reconnect with your new password, then open the page again. The same password is the sign-in (user name admin) if you reach the device through your home network.</p>
<div class="msg err" id="err"></div><div class="msg ok" id="ok"></div>
<label>WiFi password</label><input type="password" id="p" autocomplete="new-password" placeholder="Min. 8 characters">
<label>Confirm password</label><input type="password" id="p2" autocomplete="new-password" placeholder="Repeat password">
<button id="btn" onclick="go()">Set WiFi password</button></div>
<script>
async function go(){var p=document.getElementById('p').value,p2=document.getElementById('p2').value,err=document.getElementById('err'),ok=document.getElementById('ok');
err.style.display=ok.style.display='none';
if(p.length<8){err.textContent='Password must be at least 8 characters.';err.style.display='block';return;}
if(p!==p2){err.textContent='Passwords do not match.';err.style.display='block';return;}
document.getElementById('btn').disabled=true;
try{var r=await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({password:p})});var j=await r.json();
if(j.ok&&j.passwordSet){ok.textContent='WiFi password set. The AP is restarting - reconnect with your new password, then reopen this page.';ok.style.display='block';}
else{err.textContent=j.error||'Setup failed.';err.style.display='block';document.getElementById('btn').disabled=false;}
}catch(e){ok.textContent='WiFi password set. The AP is restarting - reconnect with your new password, then reopen this page.';ok.style.display='block';}}
</script></body></html>)HTML";

bool isDeviceProvisioned()
{
  return wifi_password_provisioned(wifiPassword);
}

static bool requestViaSta(AsyncWebServerRequest *request)
{
  AsyncClient *c = request->client();
  uint32_t local = c ? (uint32_t)c->localIP() : 0;
  return access_via_sta(local, (uint32_t)WiFi.softAPIP());
}

static access_decision_t decideRequest(AsyncWebServerRequest *request)
{
  const bool viaSta = requestViaSta(request);
  const bool provisioned = isDeviceProvisioned();
  const char *url = request->url().c_str();
  const bool creds = viaSta && provisioned && request->authenticate("admin", wifiPassword);
  const bool openPath = access_is_setup_path(url) || is_captive_probe(url);
  return access_decide(viaSta, provisioned, creds, openPath);
}

class AccessGate : public AsyncWebHandler
{
public:
  bool canHandle(AsyncWebServerRequest *request) override
  {
    return decideRequest(request) != ACCESS_ALLOW;
  }

  void handleRequest(AsyncWebServerRequest *request) override
  {
    switch (decideRequest(request))
    {
    case ACCESS_SETUP_REDIRECT:
      if (request->method() == HTTP_GET || request->method() == HTTP_HEAD)
        request->redirect("/setup");
      else
        request->send(403, "application/json", "{\"ok\":false,\"error\":\"Set a WiFi password first\"}");
      break;
    case ACCESS_DENY_UNPROVISIONED:
      request->send(403, "text/plain", "Set a WiFi password on the device's own WiFi network first.");
      break;
    case ACCESS_CHALLENGE:
      request->requestAuthentication("OpenHaldex", false); // Basic, 401 + WWW-Authenticate
      break;
    default:
      request->send(500, "text/plain", "Access state changed - retry");
      break;
    }
  }
};

void setupWebAccess()
{
  webServer.addHandler(new AccessGate());

  // First-run page. Closes (back to /) once a password exists.
  webServer.on("/setup", HTTP_GET, [](AsyncWebServerRequest *request)
               {
                 if (isDeviceProvisioned())
                 {
                   request->redirect("/");
                   return;
                 }
                 request->send(200, "text/html", FPSTR(SETUP_PAGE)); });
}

bool analyzerInjectionPermitted(bool network, uint32_t clientLocalIp)
{
  const bool viaSta = network && access_via_sta(clientLocalIp, (uint32_t)WiFi.softAPIP());
  return access_analyzer_injection(isDeviceProvisioned(), viaSta);
}
