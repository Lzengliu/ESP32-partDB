#include "http_server.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "app_config.h"
#include "board_config.h"
#include "button_input.h"
#include "camera_mgr.h"
#include "device_ui.h"
#include "display_ili9488.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "hardware_diag.h"
#include "nfc_pn532.h"
#include "nfc_i2c.h"
#include "nfc_service.h"
#include "partdb_client.h"
#include "storage_sd.h"
#include "qr_scanner.h"
#include "touch_ft6336.h"
#include "ui_font.h"
#include "wifi_portal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

static const char *TAG = "http";
static httpd_handle_t s_server;
static app_config_t *s_cfg;

#define HTTP_SD_PATH_MAX 384
#define ASSET_NAME_MAX 96
#define ASSET_SCAN_MAX 24
#define ASSET_SCREEN_BG_LIMIT 5
#define ASSET_LOCK_BG_LIMIT 5
#define ASSET_BOOT_ANIM_LIMIT 1
#define FONT_UPLOAD_MAX_BYTES (32 * 1024 * 1024)
#define PARTDB_RESPONSE_MAX 8192
#define NFC_BARCODE_MAX 128

typedef struct {
    const char *kind;
    const char *label;
    const char *rel_dir;
    int limit;
    bool animated;
} asset_kind_t;

typedef struct {
    char name[ASSET_NAME_MAX];
    char path[HTTP_SD_PATH_MAX];
    char type[12];
    size_t size;
    long mtime;
} asset_entry_t;

typedef struct {
    char prefix;
    int id;
    char type[16];
    char barcode[NFC_BARCODE_MAX];
    char api_path[80];
    char scan_path[64];
} partdb_barcode_target_t;

static const asset_kind_t ASSET_KINDS[] = {
    {.kind = "screen_bg", .label = "屏幕背景图", .rel_dir = "/backgrounds", .limit = ASSET_SCREEN_BG_LIMIT, .animated = false},
    {.kind = "boot_anim", .label = "开机动态图片", .rel_dir = "/boot/animation", .limit = ASSET_BOOT_ANIM_LIMIT, .animated = true},
    {.kind = "lock_bg", .label = "锁屏背景图", .rel_dir = "/lockscreen", .limit = ASSET_LOCK_BG_LIMIT, .animated = false},
};

static void nfc_manual_request_begin(void);
static void nfc_manual_request_end(void);

static const char INDEX_HTML[] =
"<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>Part-DB Terminal</title><style>:root{font-family:system-ui,-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;color:#1f2933;background:#f4f6f8}body{margin:0;letter-spacing:0}"
".shell{max-width:1080px;margin:0 auto;padding:18px}.top{display:flex;justify-content:space-between;gap:12px;align-items:flex-start;margin-bottom:14px}"
"h1{font-size:24px;margin:0}h2{font-size:17px;margin:0 0 12px}label{display:block;margin:10px 0 4px;font-size:13px;color:#52606d}"
"input,select{width:100%;padding:10px;border:1px solid #cbd2d9;border-radius:6px;box-sizing:border-box;background:#fff}input[type=checkbox]{width:auto}"
"button{border:1px solid #9fb3c8;background:#fff;color:#1f2933;border-radius:6px;padding:9px 12px;margin:4px 6px 4px 0}button.primary{background:#2563eb;color:#fff;border-color:#2563eb}button.danger{border-color:#b91c1c;color:#b91c1c}"
".tabs{display:flex;gap:6px;flex-wrap:wrap;margin:10px 0 16px}.tabs button.active{background:#102a43;color:#fff;border-color:#102a43}"
".panel{display:none}.panel.active{display:block}.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:14px}.box{background:#fff;border:1px solid #d9e2ec;border-radius:8px;padding:14px}"
".wide{grid-column:1/-1}.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px}.metric{background:#fff;border:1px solid #d9e2ec;border-radius:8px;padding:12px;min-height:64px}.metric b{display:block;font-size:13px;color:#52606d}.metric span{display:block;margin-top:6px;font-size:15px}"
".inline{display:flex;gap:10px;align-items:center}.inline input,.inline select{min-width:0}.msg{min-height:22px;color:#0f7b0f}.muted{color:#66788a;font-size:13px}.result{background:#111827;color:#e5e7eb;border-radius:8px;padding:12px;overflow:auto;min-height:120px;white-space:pre-wrap}.api-log{display:none}.bar{height:10px;background:#d9e2ec;border-radius:99px;overflow:hidden}.bar i{display:block;height:100%;width:0;background:#2563eb}"
".camview img{display:block;max-width:100%;height:auto;margin-top:8px;border:1px solid #d9e2ec;border-radius:6px}.scanbox{border:1px solid #d9e2ec;border-radius:8px;background:#f8fafc;padding:10px;margin-top:8px}.scanbox b{display:block}.scanbox code{display:block;word-break:break-all;margin-top:6px;color:#102a43}"
".assetgrid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:14px;margin:14px 0}.assetcard{background:#fff;border:1px solid #d9e2ec;border-radius:8px;padding:12px}.assetpreview{position:relative;min-height:170px;background:#f4f6f8;border:1px solid #d9e2ec;border-radius:6px;display:flex;align-items:center;justify-content:center;overflow:hidden}.assetpreview img{max-width:100%;max-height:220px}.assetdel{position:absolute;right:8px;bottom:8px;background:#fff;color:#b91c1c;border-color:#b91c1c}.modal{position:fixed;inset:0;background:rgba(15,23,42,.45);display:flex;align-items:center;justify-content:center;padding:18px;z-index:10}.modal.hidden{display:none}.dialog{background:#fff;border-radius:8px;border:1px solid #d9e2ec;padding:16px;max-width:420px;width:100%;box-shadow:0 20px 45px rgba(15,23,42,.24)}"
".statusrow{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px}.statusitem{border:1px solid #d9e2ec;border-radius:8px;padding:10px;background:#fff}.statusitem b{display:block;font-size:13px;color:#52606d}.statusitem span{display:block;margin-top:6px}.ok{color:#0f7b0f}.bad{color:#b91c1c}.wait{color:#8a6d1d}"
".browser{display:block}.pathbar{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:10px}.filegrid{display:grid;grid-template-columns:repeat(auto-fill,minmax(130px,1fr));gap:10px}.file{border:1px solid #d9e2ec;background:#fff;border-radius:8px;padding:10px;text-align:left;min-height:84px}.file .ico{font-size:12px;font-weight:700;color:#2563eb}.file .name{display:block;word-break:break-all;margin-top:8px}.file .meta{font-size:12px;color:#66788a;margin-top:4px}.preview{background:#fff;border:1px solid #d9e2ec;border-radius:8px;padding:12px;min-height:220px}.preview img{max-width:100%;height:auto;border-radius:6px;border:1px solid #d9e2ec}"
"@media(max-width:760px){.top{display:block}.grid,.cards,.assetgrid{grid-template-columns:1fr}.shell{padding:12px}}</style></head><body><div class=\"shell\">"
"<div class=\"top\"><div><h1 id=\"device_title\">Part-DB Terminal</h1><div id=\"headline\" class=\"muted\">loading...</div></div><button onclick=\"loadAll()\">刷新</button></div>"
"<div class=\"cards\"><div class=\"metric\"><b>WiFi</b><span id=\"m_wifi\">-</span></div><div class=\"metric\"><b>AP</b><span id=\"m_ap\">-</span></div><div class=\"metric\"><b>Part-DB</b><span id=\"m_partdb\">-</span></div><div class=\"metric\"><b>TF 卡</b><span id=\"m_tf\">-</span></div><div class=\"metric\"><b>硬件</b><span id=\"m_hw\">-</span></div></div>"
"<div class=\"tabs\"><button class=\"active\" data-tab=\"overview\" onclick=\"tab('overview')\">总览</button><button data-tab=\"settings\" onclick=\"tab('settings')\">设置</button><button data-tab=\"partdb\" onclick=\"tab('partdb')\">Part-DB</button><button data-tab=\"hardware\" onclick=\"tab('hardware')\">硬件</button><button data-tab=\"resources\" onclick=\"tab('resources')\">资源</button><button data-tab=\"maintenance\" onclick=\"tab('maintenance')\">维护</button></div>"
"<section id=\"overview\" class=\"panel active\"><div class=\"grid\"><div class=\"box\"><h2>运行状态</h2><div id=\"status_cards\" class=\"statusrow\"></div><pre id=\"status\" class=\"api-log\"></pre></div><div class=\"box\"><h2>快速操作</h2><button onclick=\"enableAp()\">开启 AP</button><button onclick=\"mountSd()\">挂载 TF</button><button onclick=\"captureCamera()\">抓拍</button><button class=\"primary\" onclick=\"scanQr()\">扫码</button><p id=\"msg\" class=\"msg\"></p><div id=\"qr_result\"></div><div id=\"camera_preview\" class=\"camview\"></div></div></div></section>"
"<section id=\"settings\" class=\"panel\"><div class=\"grid\"><div class=\"box\"><h2>网络</h2><label>设备名称</label><input id=\"device_name\" data-cfg maxlength=\"47\"><label>WiFi SSID</label><input id=\"wifi_ssid\" data-cfg><label>WiFi 密码</label><input id=\"wifi_pass\" data-cfg type=\"password\" placeholder=\"留空则不修改已保存密码\"><label class=\"inline\"><input id=\"ap_enabled\" data-cfg type=\"checkbox\">启用配置 AP</label><button class=\"primary\" onclick=\"saveCfg()\">保存并重连</button></div>"
"<div class=\"box\"><h2>资源路径</h2><label>开机图路径</label><p id=\"fixed_boot_image_path\" class=\"muted\"></p><label>字体目录</label><p id=\"fixed_font_dir\" class=\"muted\"></p><label>字体文件</label><select id=\"font_path\" data-cfg onchange=\"fontSelectionChanged()\"><option value=\"\">默认内置字体</option></select><input id=\"font_file\" type=\"file\" accept=\".ttf,.otf,.ttc,.woff,.woff2\"><button onclick=\"uploadFont()\">上传字体</button><button onclick=\"loadFonts()\">刷新字体</button><button id=\"font_delete\" class=\"danger\" onclick=\"deleteFont()\">删除所选字体</button><button class=\"primary\" onclick=\"saveResourceCfg()\">保存字体设置</button><p id=\"font_msg\" class=\"msg\"></p><p id=\"font_count\" class=\"muted\"></p></div>"
"<div class=\"box\"><h2>显示</h2><label>显示驱动</label><select id=\"display_driver\" data-cfg><option value=\"ili9488\">ILI9488 SPI</option><option value=\"st7789\">ST7789 SPI（预留）</option><option value=\"ili9341\">ILI9341 SPI（预留）</option></select><label>屏幕尺寸</label><select id=\"display_preset\" onchange=\"setDisplayPreset(this.value)\"><option value=\"320x480\">320 x 480</option><option value=\"480x320\">480 x 320</option><option value=\"240x320\">240 x 320</option><option value=\"240x240\">240 x 240</option><option value=\"custom\">自定义</option></select><div class=\"inline\"><input id=\"display_width\" data-cfg type=\"number\" min=\"64\" max=\"1024\"><input id=\"display_height\" data-cfg type=\"number\" min=\"64\" max=\"1024\"></div><label>方向</label><select id=\"display_orientation\" data-cfg><option value=\"portrait\">竖屏</option><option value=\"landscape\">横屏</option></select><label class=\"inline\"><input id=\"display_flip\" data-cfg type=\"checkbox\">翻转 180°</label><label>触摸旋转</label><select id=\"touch_rotation\" data-cfg><option value=\"0\">0°</option><option value=\"1\">90°</option><option value=\"2\">180°</option><option value=\"3\">270°</option></select><label>触摸原始范围</label><div class=\"inline\"><input id=\"touch_raw_width\" data-cfg type=\"number\" min=\"64\" max=\"1024\"><input id=\"touch_raw_height\" data-cfg type=\"number\" min=\"64\" max=\"1024\"></div><label class=\"inline\"><input id=\"touch_swap_xy\" data-cfg type=\"checkbox\">交换触摸 X/Y</label><label class=\"inline\"><input id=\"touch_flip_x\" data-cfg type=\"checkbox\">触摸 X 翻转</label><label class=\"inline\"><input id=\"touch_flip_y\" data-cfg type=\"checkbox\">触摸 Y 翻转</label><label>屏幕亮度 <span id=\"brightness_value\"></span></label><input id=\"display_brightness\" data-cfg type=\"range\" min=\"0\" max=\"100\" step=\"5\" oninput=\"brightness_value.textContent=this.value+'%'\" onchange=\"setBrightness(this.value)\"></div></div></section>"
"<section id=\"partdb\" class=\"panel\"><div class=\"grid\"><div class=\"box\"><h2>接口配置</h2><label>Part-DB 地址</label><input id=\"partdb_url\" data-cfg placeholder=\"http://partdb.local 或 http://partdb.local/api\"><label>Part-DB API Token</label><input id=\"partdb_token\" data-cfg type=\"password\"><button class=\"primary\" onclick=\"savePartdbCfg()\">保存接口</button><button onclick=\"partdbGet('/api/parts.jsonld?itemsPerPage=1')\">测试元件列表</button><button onclick=\"partdbGet('/api/categories.jsonld?itemsPerPage=1')\">测试分类</button><p id=\"partdb_msg\" class=\"msg\"></p><p class=\"muted\">Token 在 Part-DB 用户设置 / API Tokens 中创建；测试路径固定隐藏，避免误填。</p><pre id=\"partdb_out\" class=\"api-log\"></pre></div></div></section>"
"<section id=\"hardware\" class=\"panel\"><div class=\"box\"><h2>硬件诊断</h2><div id=\"hw_cards\" class=\"statusrow\"></div><button class=\"primary\" onclick=\"runHardwareDiag()\">重新诊断</button><button onclick=\"diag('/api/display/test')\">显示测试</button><button onclick=\"diag('/api/touch')\">读取触摸</button><button onclick=\"diag('/api/nfc/read')\">读取 NFC</button><button onclick=\"mountSd()\">挂载 TF</button><button onclick=\"captureCamera()\">抓拍</button><button class=\"primary\" onclick=\"scanQr()\">扫码</button><p id=\"hw_msg\" class=\"msg\"></p><div id=\"hw_qr_result\"></div><div id=\"hw_camera_preview\" class=\"camview\"></div><pre id=\"hw_out\" class=\"api-log\"></pre></div></section>"
"<section id=\"resources\" class=\"panel\"><div class=\"grid\"><div class=\"box\"><h2>TF 卡</h2><div class=\"bar\"><i id=\"sd_bar\"></i></div><p id=\"sd_text\" class=\"muted\">未挂载</p><button onclick=\"mountSd()\">挂载/刷新</button><button onclick=\"prepareSd()\">初始化目录</button><button class=\"danger\" onclick=\"formatSd()\">格式化 TF</button><p class=\"muted\">格式化会清空整张卡；固件只把 TF 卡用于缓存、屏保、开机动画和字体。</p></div>"
"<div class=\"box\"><h2>上传资源</h2><label>资源类型</label><select id=\"asset_kind\" onchange=\"assetKindChanged()\"><option value=\"screen_bg\">屏幕背景图</option><option value=\"boot_anim\">开机动态图片</option><option value=\"lock_bg\">锁屏背景图</option></select><input id=\"asset_file\" type=\"file\" accept=\"image/*\"><label id=\"fit_label\">图片适配</label><select id=\"fit_mode\"><option value=\"contain\">完整显示</option><option value=\"cover\">铺满裁切</option></select><button class=\"primary\" onclick=\"uploadAsset()\">上传</button><p id=\"asset_msg\" class=\"msg\"></p></div></div>"
"<div id=\"asset_cards\" class=\"assetgrid\"></div><div class=\"browser\"><div class=\"box\" hidden><h2>文件</h2><div class=\"pathbar\"><button onclick=\"openSd('/')\">根目录</button><button onclick=\"openParent()\">上一级</button><span id=\"sd_path\" class=\"muted\">/</span></div><div id=\"sd_files\" class=\"filegrid\"></div><p id=\"res_msg\" class=\"msg\"></p><pre id=\"res_out\" class=\"api-log\"></pre></div><div class=\"preview\"><h2>预览</h2><div class=\"muted\">boot_image_path</div><p id=\"path_boot\" class=\"muted\"></p><div class=\"muted\">font_dir</div><p id=\"path_font\" class=\"muted\"></p><div class=\"muted\">font_path</div><p id=\"path_font_file\" class=\"muted\"></p><div id=\"file_preview\"></div></div></div></section>"
"<section id=\"maintenance\" class=\"panel\"><div class=\"grid\"><div class=\"box\"><h2>OTA</h2><input id=\"ota\" type=\"file\"><button class=\"primary\" onclick=\"otaUpload()\">上传固件</button><p id=\"ota_msg\" class=\"msg\"></p></div><div class=\"box\"><h2>项目声明</h2><p id=\"dev_line\" class=\"muted\">-</p><p id=\"repo_line\" class=\"muted\">-</p><pre id=\"api_out\" class=\"api-log\">/api/status\n/api/config\n/api/sd/format\n/api/sd/upload\n/api/partdb/get</pre></div></div></section>"
"<section id=\"future\" hidden><button onclick=\"screenPower(false)\">息屏</button><button onclick=\"screenPower(true)\">唤醒</button></section><div id=\"confirm_modal\" class=\"modal hidden\"><div class=\"dialog\"><h2>确认操作</h2><p id=\"confirm_text\" class=\"muted\"></p><button id=\"confirm_ok\" class=\"danger\">确认</button><button id=\"confirm_cancel\">取消</button></div></div>"
"<script>"
"let editing=false,loaded=false,lastConfig={};const cfgFields=['device_name','wifi_ssid','partdb_url','font_path','display_driver','display_orientation','display_width','display_height','touch_rotation','touch_raw_width','touch_raw_height'];"
"function $(id){return document.getElementById(id)}function say(t){$('msg').textContent=t||''}function tab(id){document.querySelectorAll('.panel').forEach(p=>p.classList.toggle('active',p.id===id));document.querySelectorAll('.tabs button').forEach(b=>b.classList.toggle('active',b.dataset.tab===id))}"
"async function j(u,o){let r=await fetch(u,o),t=await r.text(),d={};try{d=t?JSON.parse(t):{}}catch(e){d={ok:false,err:t||('HTTP '+r.status)}}if(!r.ok&&d.ok!==false){d.ok=false;d.err=d.err||('HTTP '+r.status)}d.http_status=r.status;return d}document.addEventListener('input',e=>{if(e.target&&e.target.dataset.cfg!==undefined)editing=true;if(e.target&&(e.target.id==='display_width'||e.target.id==='display_height'))syncDisplayPreset()});"
"function syncDisplayPreset(){let p=$('display_preset'),v=($('display_width').value||'')+'x'+($('display_height').value||''),found=false;Array.from(p.options).forEach(o=>{if(o.value===v)found=true});p.value=found?v:'custom'}function setDisplayPreset(v){if(v==='custom')return;let a=v.split('x');$('display_width').value=a[0];$('display_height').value=a[1];editing=true}"
"async function loadConfig(){let c=await j('/api/config');lastConfig=c;if(!editing&&!loaded){cfgFields.forEach(k=>{let e=$(k);if(e)e.value=(c[k]===undefined?'':c[k])});$('ap_enabled').checked=!!c.ap_enabled;$('display_flip').checked=!!c.display_flip;$('touch_swap_xy').checked=!!c.touch_swap_xy;$('touch_flip_x').checked=!!c.touch_flip_x;$('touch_flip_y').checked=!!c.touch_flip_y;$('display_brightness').value=c.display_brightness||0;$('brightness_value').textContent=(c.display_brightness||0)+'%';syncDisplayPreset();loaded=true}$('device_title').textContent=c.device_name||'Part-DB Terminal';$('fixed_boot_image_path').textContent=c.boot_image_path||'';$('fixed_font_dir').textContent=c.font_dir||'';$('path_boot').textContent=c.boot_image_path||'';$('path_font').textContent=c.font_dir||'';$('path_font_file').textContent=c.font_path||'默认内置字体'}"
"function fmtMB(n){return n>=1024?(n/1024).toFixed(1)+' GB':Number(n||0).toFixed(1)+' MB'}function renderSd(sd){sd=sd||{};let total=sd.total_mb||sd.card_size_mb||0,free=sd.free_mb||0,used=Math.max(0,total-free),pct=total?Math.min(100,used*100/total):0;$('sd_bar').style.width=pct+'%';$('sd_text').textContent=sd.mounted?('已挂载 '+fmtMB(free)+' 可用 / '+fmtMB(total)+' 总容量'):'未挂载';$('m_tf').textContent=sd.mounted?fmtMB(free)+' 可用':'未挂载'}"
"function item(label,it){it=it||{};let cls=it.ok?'ok':(it.err==='ESP_ERR_INVALID_STATE'?'wait':'bad'),txt=it.ok?'正常':(it.err==='ESP_ERR_INVALID_STATE'?'检测中':'异常');return `<div class=\"statusitem\"><b>${label}</b><span class=\"${cls}\">${txt}</span></div>`}"
"function renderHw(h){h=h||{};let d=h.diagnostics||{},html=item('显示屏',d.display)+item('触摸',d.touch)+item('NFC',d.nfc)+item('TF 卡',d.tf)+item('相机',d.camera);$('hw_cards').innerHTML=html;let vals=[d.display,d.touch,d.nfc,d.tf,d.camera].filter(Boolean),ok=vals.filter(x=>x.ok).length,bad=vals.filter(x=>x.err&&x.err!=='ESP_OK'&&x.err!=='ESP_ERR_INVALID_STATE').length;$('m_hw').textContent=d.running?'诊断中':(ok+' 正常 / '+bad+' 异常');return html}"
"function renderStatus(s){$('status').textContent=JSON.stringify(s,null,2);$('device_title').textContent=s.device_name||'Part-DB Terminal';$('headline').textContent=(s.wifi&&s.wifi.ip)?('STA '+s.wifi.ip):'AP 192.168.4.1';$('m_wifi').textContent=s.wifi&&s.wifi.sta_connected?s.wifi.ip:'未连接';$('m_ap').textContent=s.wifi&&s.wifi.ap_started?'已开启':'关闭';$('m_partdb').textContent=s.partdb&&s.partdb.configured?(s.partdb.last_ok?'在线':'已配置'):'未配置';renderSd(s.sd);let hw=renderHw(s.hardware);$('status_cards').innerHTML=item('WiFi',{ok:s.wifi&&s.wifi.sta_connected,err:s.wifi&&s.wifi.sta_connected?'ESP_OK':'ESP_FAIL'})+item('Part-DB',{ok:s.partdb&&s.partdb.configured,err:s.partdb&&s.partdb.configured?'ESP_OK':'ESP_FAIL'})+item('TF 卡',{ok:s.sd&&s.sd.mounted,err:s.sd&&s.sd.mounted?'ESP_OK':'ESP_FAIL'})+hw;let a=s.about||{};$('dev_line').textContent='开发者：'+(a.developer||'-');$('repo_line').textContent=a.source_repo_reserved?'源码仓库：预留':'源码仓库：'+a.source_repo_url}"
"async function loadStatus(){renderStatus(await j('/api/status'))}async function loadAll(){await loadConfig();await loadFonts();assetKindChanged();await loadStatus();await loadAssets()}async function savePartialConfig(body){return await j('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})}async function saveCfg(){let body={};['device_name','wifi_ssid','wifi_pass','partdb_url','partdb_token','font_path','display_driver','display_orientation'].forEach(k=>body[k]=$(k).value);body.ap_enabled=$('ap_enabled').checked;body.display_flip=$('display_flip').checked;body.touch_rotation=Number($('touch_rotation').value)||0;body.touch_swap_xy=$('touch_swap_xy').checked;body.touch_flip_x=$('touch_flip_x').checked;body.touch_flip_y=$('touch_flip_y').checked;body.display_brightness=Number($('display_brightness').value);body.display_width=Number($('display_width').value)||320;body.display_height=Number($('display_height').value)||480;body.touch_raw_width=Number($('touch_raw_width').value)||320;body.touch_raw_height=Number($('touch_raw_height').value)||480;let r=await savePartialConfig(body);say(r.ok?'已保存，正在重连':'保存失败 '+r.err);editing=false;loaded=false;$('wifi_pass').value='';$('partdb_token').value='';setTimeout(loadAll,900)}"
"async function saveResourceCfg(){let body={font_path:$('font_path').value};let r=await savePartialConfig(body);$('font_msg').textContent=r.ok?'字体设置已保存':'保存失败 '+r.err;editing=false;loaded=false;await loadConfig();await loadFonts()}"
"async function setBrightness(v){$('brightness_value').textContent=v+'%';await j('/api/display/brightness',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({display_brightness:Number(v)})});loadStatus()}async function enableAp(){let r=await j('/api/wifi/ap',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({enabled:true})});say(r.ok?'AP 已开启':'开启 AP 失败 '+r.err);loadStatus()}"
"async function screenPower(awake){await j('/api/display/power',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({awake:!!awake})});loadStatus()}async function diag(u){let r=await j(u);$('hw_out').textContent=JSON.stringify(r,null,2);$('hw_msg').textContent=r.ok===false?('执行失败 '+r.err):'已执行，详细日志看串口';loadStatus()}async function runHardwareDiag(){$('hw_msg').textContent='正在诊断...';let r=await j('/api/hardware/diagnose',{method:'POST'});$('hw_out').textContent=JSON.stringify(r,null,2);$('hw_msg').textContent=r.ok?'诊断完成':'诊断失败 '+r.err;loadStatus()}function renderQr(r){r=r||{};let found=!!r.found,meta=(r.width||0)+'x'+(r.height||0)+' · '+(r.elapsed_ms||0)+' ms · '+(r.err||'');let html='<div class=\"scanbox\"><b>'+(found?'识别到二维码':'未识别到二维码')+'</b>'+(found?'<code>'+esc(r.text||'')+'</code>':'')+'<div class=\"muted\">'+esc(meta)+'</div></div>';$('qr_result').innerHTML=html;$('hw_qr_result').innerHTML=html}async function scanQr(){say('正在扫码...');$('hw_msg').textContent='正在扫码...';try{let r=await j('/api/camera/scan',{method:'POST'});renderQr(r);let msg=r.found?'扫码成功':(r.ok?'未识别到二维码':'扫码失败 '+r.err);say(msg);$('hw_msg').textContent=msg}catch(e){say('扫码失败 '+e.message);$('hw_msg').textContent='扫码失败 '+e.message}loadStatus()}async function captureCamera(){say('正在抓拍...');$('hw_msg').textContent='正在抓拍...';try{let r=await fetch('/api/camera.jpg?t='+Date.now());if(!r.ok)throw new Error('HTTP '+r.status);let b=await r.blob(),u=URL.createObjectURL(b);if(window.cameraUrl)URL.revokeObjectURL(window.cameraUrl);window.cameraUrl=u;let html='<img src=\"'+u+'\" alt=\"camera\">';$('camera_preview').innerHTML=html;$('hw_camera_preview').innerHTML=html;say('抓拍已刷新');$('hw_msg').textContent='抓拍已刷新'}catch(e){say('抓拍失败 '+e.message);$('hw_msg').textContent='抓拍失败 '+e.message}loadStatus()}async function otaUpload(){let f=$('ota').files[0];if(!f){$('ota_msg').textContent='请选择固件';return}let r=await fetch('/api/ota',{method:'POST',body:f});$('ota_msg').textContent=await r.text()}"
"async function savePartdbCfg(){let body={partdb_url:$('partdb_url').value,partdb_token:$('partdb_token').value};let r=await savePartialConfig(body);$('partdb_msg').textContent=r.ok?'接口已保存':'保存失败 '+r.err;$('partdb_token').value='';editing=false;loaded=false;loadStatus()}async function partdbGet(path){let r=await j('/api/partdb/get',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path:path})});$('partdb_out').textContent=JSON.stringify(r,null,2);$('partdb_msg').textContent=r.ok?'测试通过，详细响应看串口':'测试失败 HTTP '+(r.http_status||0)+' '+r.err;loadStatus()}"
"function fontSelectionChanged(){let p=$('font_path').value;$('font_delete').disabled=!p;$('path_font_file').textContent=p||'默认内置字体'}"
"async function loadFonts(){let sel=$('font_path'),cur=lastConfig.font_path||sel.value||'';try{let r=await j('/api/fonts/list');let html='<option value=\"\">默认内置字体</option>';(r.entries||[]).forEach(e=>{html+='<option value=\"'+esc(e.path)+'\">'+esc(e.name)+' · '+fmt(e.size)+'</option>'});sel.innerHTML=html;sel.value=cur;if(sel.value!==cur)sel.value='';fontSelectionChanged();$('font_count').textContent=r.ok?((r.count||0)+' 个字体文件'):'读取字体目录失败 '+r.err}catch(e){$('font_count').textContent='读取字体目录失败 '+e.message;fontSelectionChanged()}}"
"async function uploadFont(){let f=$('font_file').files[0];if(!f){$('font_msg').textContent='请选择字体文件';return}let n=f.name.toLowerCase();if(!(/\\.(ttf|otf|ttc|woff|woff2)$/.test(n))){$('font_msg').textContent='字体只支持 TTF/OTF/TTC/WOFF/WOFF2';return}$('font_msg').textContent='正在上传字体 '+fmt(f.size)+'，请等待';try{let r=await j('/api/fonts/upload?name='+encodeURIComponent(f.name),{method:'POST',body:f});$('res_out').textContent=JSON.stringify(r,null,2);if(r.ok){lastConfig.font_path=r.path;await loadFonts();$('font_path').value=r.path;fontSelectionChanged();editing=true;$('font_msg').textContent='已上传 '+r.path+'，大小 '+fmt(r.size)+'，点击保存字体设置生效'}else{$('font_msg').textContent='上传失败 '+(r.err||('HTTP '+(r.http_status||0)))}}catch(e){$('font_msg').textContent='上传失败 '+e.message}}"
"async function deleteFont(){let p=$('font_path').value;if(!p){$('font_msg').textContent='请选择要删除的字体';return}showConfirm('确认删除字体 '+p+'？',async()=>{let r=await j('/api/fonts/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path:p})});if(r.ok){lastConfig.font_path='';$('font_path').value='';$('font_msg').textContent='字体已删除';$('path_font_file').textContent='默认内置字体';editing=false;loaded=false;await loadConfig();await loadFonts()}else{$('font_msg').textContent='删除失败 '+r.err}})}"
"let sdPath='/';function fmt(n){if(n>1048576)return(n/1048576).toFixed(1)+' MB';if(n>1024)return(n/1024).toFixed(1)+' KB';return n+' B'}function esc(s){return String(s).replace(/[&<>\"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[c]))}"
"async function mountSd(){let r=await j('/api/sd/mount');$('res_out').textContent=JSON.stringify(r,null,2);$('res_msg').textContent=r.ok?'TF 已挂载':'TF 挂载失败 '+r.err;renderSd(r);await openSd(sdPath);await loadAssets();loadStatus()}async function prepareSd(){let r=await j('/api/sd/prepare',{method:'POST'});$('res_out').textContent=JSON.stringify(r,null,2);$('res_msg').textContent=r.ok?'目录已初始化':'初始化失败 '+r.err;renderSd(r);await openSd('/');await loadAssets();loadStatus()}async function formatSd(){if(!confirm('格式化会清空整张 TF 卡。继续？'))return;let r=await j('/api/sd/format',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({confirm:'FORMAT'})});$('res_out').textContent=JSON.stringify(r,null,2);$('res_msg').textContent=r.ok?'TF 已格式化':'格式化失败 '+r.err;renderSd(r);await openSd('/');await loadAssets();loadStatus()}async function openParent(){if(sdPath==='/'||!sdPath)return;let p=sdPath.endsWith('/')?sdPath.slice(0,-1):sdPath;p=p.substring(0,p.lastIndexOf('/'))||'/';openSd(p)}"
"async function openSd(p){sdPath=p||'/';$('sd_path').textContent=sdPath;let r=await j('/api/sd/list',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path:sdPath})});$('res_out').textContent=JSON.stringify({ok:r.ok,err:r.err,path:r.path,total_mb:r.total_mb,free_mb:r.free_mb},null,2);if(!r.ok){$('res_msg').textContent='读取目录失败 '+r.err}renderSd(r);let html='';(r.entries||[]).forEach(e=>{let ico=e.dir?'DIR':(e.type==='image'?'IMG':(e.type==='text'?'TXT':'FILE'));let act=e.dir?`openSd(${JSON.stringify(e.path)})`:`previewFile(${JSON.stringify(e.path)},${JSON.stringify(e.type)})`;html+=`<button class=\"file\" onclick=\"${act}\"><span class=\"ico\">${ico}</span><span class=\"name\">${esc(e.name)}</span><span class=\"meta\">${e.dir?'目录':fmt(e.size)}</span></button>`});$('sd_files').innerHTML=html||'<div class=\"muted\">没有文件</div>'}"
"async function previewFile(p,t){let u='/api/sd/file?path='+encodeURIComponent(p);if(t==='image'){$('file_preview').innerHTML='<img src=\"'+u+'\"><p class=\"muted\">'+esc(p)+'</p>'}else if(t==='text'){let txt=await(await fetch(u)).text();$('file_preview').innerHTML='<pre class=\"result\">'+esc(txt.slice(0,4096))+'</pre>'}else{$('file_preview').innerHTML='<p>'+esc(p)+'</p><a href=\"'+u+'\" target=\"_blank\">下载/打开</a>'}}"
"const assetKinds=[{kind:'screen_bg',label:'屏幕背景图'},{kind:'boot_anim',label:'开机动态图片'},{kind:'lock_bg',label:'锁屏背景图'}];let pendingConfirm=null;"
"function safeName(n){return String(n||'asset').replace(/\\.[^.]*$/,'').replace(/[^A-Za-z0-9_-]+/g,'_').slice(0,40)||'asset'}function loadImg(f){return new Promise((ok,fail)=>{let u=URL.createObjectURL(f),im=new Image();im.onload=()=>{URL.revokeObjectURL(u);ok(im)};im.onerror=fail;im.src=u})}"
"function assetKindChanged(){let boot=$('asset_kind').value==='boot_anim';$('asset_file').accept=boot?'image/gif,image/webp':'image/*';$('fit_label').style.display=boot?'none':'block';$('fit_mode').style.display=boot?'none':'block';$('asset_msg').textContent=boot?'开机动态图片只保存 GIF/WebP，限 1 份':''}"
"function showConfirm(text,cb){pendingConfirm=cb;$('confirm_text').textContent=text;$('confirm_modal').classList.remove('hidden')}function closeConfirm(){pendingConfirm=null;$('confirm_modal').classList.add('hidden')}"
"function displaySize(){let w=Number($('display_width').value||lastConfig.display_width||320),h=Number($('display_height').value||lastConfig.display_height||480);return{w:w||320,h:h||480}}"
"async function imageBlob(f){let im=await loadImg(f),sz=displaySize(),cw=sz.w,ch=sz.h,c=document.createElement('canvas'),x=0,y=0,w=cw,h=ch;c.width=cw;c.height=ch;let ctx=c.getContext('2d');ctx.fillStyle='#000';ctx.fillRect(0,0,cw,ch);let cover=$('fit_mode').value==='cover',s=cover?Math.max(cw/im.width,ch/im.height):Math.min(cw/im.width,ch/im.height);w=Math.round(im.width*s);h=Math.round(im.height*s);x=Math.round((cw-w)/2);y=Math.round((ch-h)/2);ctx.drawImage(im,x,y,w,h);return await new Promise(r=>c.toBlob(r,'image/jpeg',0.86))}"
"function assetPreview(e){let u='/api/sd/file?path='+encodeURIComponent(e.path),p=encodeURIComponent(e.path),sel=e.selected?'使用中':'设为使用',dis=e.selected?' disabled':'';return `<div><div class=\"assetpreview\"><img src=\"${u}\"><button class=\"assetdel\" onclick=\"deleteAsset(decodeURIComponent('${p}'))\">删除</button></div><button class=\"primary\"${dis} onclick=\"selectAsset(decodeURIComponent('${p}'))\">${sel}</button><p class=\"muted\">${esc(e.name)} · ${fmt(e.size)}</p></div>`}"
"async function loadAssets(){let html='';for(const k of assetKinds){let r=await j('/api/assets/list?kind='+encodeURIComponent(k.kind));let entries=r.entries||[],body=entries.length?entries.map(assetPreview).join(''):'<div class=\"assetpreview\"><span class=\"muted\">未上传</span></div>';html+=`<div class=\"assetcard\"><h2>${k.label}</h2><p class=\"muted\">${r.count||0} / ${r.limit||0}${r.selected_path?' · 当前 '+esc(r.selected_path):''}</p>${body}</div>`}$('asset_cards').innerHTML=html}"
"async function selectAsset(path){let r=await j('/api/assets/select',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path:path})});$('asset_msg').textContent=r.ok?'已设为使用':'设置失败 '+r.err;if(r.ok){await loadConfig();await loadAssets()}loadStatus()}"
"async function deleteAsset(path){showConfirm('确认删除 '+path+'？',async()=>{let r=await j('/api/assets/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path:path})});$('asset_msg').textContent=r.ok?(r.selected_cleared?'已删除，并取消当前使用':'已删除'):'删除失败 '+r.err;await loadConfig();await loadAssets();await openSd(sdPath);loadStatus()})}"
"async function sendAssetUpload(kind,f,blob,replace){let u='/api/assets/upload?kind='+encodeURIComponent(kind)+'&name='+encodeURIComponent(f.name)+(replace?'&replace_oldest=1':'');let r=await j(u,{method:'POST',body:blob});$('res_out').textContent=JSON.stringify(r,null,2);if(r.limit_exceeded){let old=r.oldest||{};showConfirm((r.label||'资源')+' 已达到 '+r.limit+' 份，删除最早的 '+(old.name||'文件')+' 后继续？',()=>sendAssetUpload(kind,f,blob,true));return}$('asset_msg').textContent=r.ok?('已上传 '+r.path+'，大小 '+fmt(r.size)+'，可在预览中设为使用'):('上传失败 '+r.err);if(r.ok){await loadAssets();await openSd(r.path.substring(0,r.path.lastIndexOf('/'))||'/')}loadStatus()}"
"async function uploadAsset(){let f=$('asset_file').files[0],kind=$('asset_kind').value;if(!f){$('asset_msg').textContent='请选择文件';return}let blob=f;if(kind==='boot_anim'){let n=f.name.toLowerCase();if(!(n.endsWith('.gif')||n.endsWith('.webp'))){$('asset_msg').textContent='开机动态图片只支持 GIF/WebP';return}$('asset_msg').textContent='正在上传开机动态图片'}else{let sz=displaySize();blob=await imageBlob(f);$('asset_msg').textContent='图片已适配为 '+sz.w+'x'+sz.h+' JPEG，正在上传'}await sendAssetUpload(kind,f,blob,false)}"
"$('confirm_cancel').onclick=closeConfirm;$('confirm_ok').onclick=async()=>{let cb=pendingConfirm;closeConfirm();if(cb)await cb()};"
"loadAll();setInterval(loadStatus,5000);</script></div></body></html>";

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    if (!text) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json alloc failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, text);
    free(text);
    return ESP_OK;
}

static bool read_body(httpd_req_t *req, char *buf, size_t buf_len)
{
    if (!req || !buf || buf_len == 0 || req->content_len >= buf_len) {
        return false;
    }
    int done = 0;
    while (done < req->content_len) {
        int r = httpd_req_recv(req, buf + done, req->content_len - done);
        if (r <= 0) {
            return false;
        }
        done += r;
    }
    buf[done] = '\0';
    return true;
}

static bool hex_digit(char c, uint8_t *out)
{
    if (c >= '0' && c <= '9') {
        *out = (uint8_t)(c - '0');
        return true;
    }
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') {
        *out = (uint8_t)(c - 'a' + 10);
        return true;
    }
    return false;
}

static void url_decode_inplace(char *s)
{
    if (!s) {
        return;
    }
    char *r = s;
    char *w = s;
    while (*r) {
        if (*r == '+') {
            *w++ = ' ';
            r++;
        } else if (*r == '%' && r[1] && r[2]) {
            uint8_t hi = 0;
            uint8_t lo = 0;
            if (hex_digit(r[1], &hi) && hex_digit(r[2], &lo)) {
                *w++ = (char)((hi << 4) | lo);
                r += 3;
            } else {
                *w++ = *r++;
            }
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static esp_err_t get_query_value(httpd_req_t *req, const char *key, char *out, size_t out_len)
{
    if (!req || !key || !out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    char *query = malloc(query_len + 1);
    if (!query) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = httpd_req_get_url_query_str(req, query, query_len + 1);
    if (err == ESP_OK) {
        err = httpd_query_key_value(query, key, out, out_len);
        if (err == ESP_OK) {
            url_decode_inplace(out);
        }
    }
    free(query);
    return err;
}

static bool sd_relative_path_is_safe(const char *path)
{
    return path && path[0] == '/' && !strstr(path, "..") && !strchr(path, '\\');
}

static esp_err_t make_sd_path(const char *rel_path, char *out, size_t out_len)
{
    if (!sd_relative_path_is_safe(rel_path) || !out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int written = snprintf(out, out_len, "%s%s", BOARD_SD_MOUNT_POINT,
                           strcmp(rel_path, "/") == 0 ? "" : rel_path);
    if (written < 0 || written >= (int)out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static const char *file_type_for_name(const char *name)
{
    const char *ext = name ? strrchr(name, '.') : NULL;
    if (!ext) {
        return "binary";
    }
    ext++;
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0 ||
        strcasecmp(ext, "png") == 0 || strcasecmp(ext, "gif") == 0 ||
        strcasecmp(ext, "bmp") == 0 || strcasecmp(ext, "webp") == 0) {
        return "image";
    }
    if (strcasecmp(ext, "txt") == 0 || strcasecmp(ext, "json") == 0 ||
        strcasecmp(ext, "csv") == 0 || strcasecmp(ext, "md") == 0 ||
        strcasecmp(ext, "log") == 0 || strcasecmp(ext, "html") == 0) {
        return "text";
    }
    return "binary";
}

static const char *content_type_for_name(const char *name)
{
    const char *ext = name ? strrchr(name, '.') : NULL;
    if (!ext) {
        return "application/octet-stream";
    }
    ext++;
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcasecmp(ext, "png") == 0) {
        return "image/png";
    }
    if (strcasecmp(ext, "gif") == 0) {
        return "image/gif";
    }
    if (strcasecmp(ext, "bmp") == 0) {
        return "image/bmp";
    }
    if (strcasecmp(ext, "webp") == 0) {
        return "image/webp";
    }
    if (strcmp(file_type_for_name(name), "text") == 0) {
        return "text/plain; charset=utf-8";
    }
    return "application/octet-stream";
}

static bool font_type_allowed(const char *name)
{
    const char *ext = name ? strrchr(name, '.') : NULL;
    if (!ext) {
        return false;
    }
    ext++;
    return strcasecmp(ext, "ttf") == 0 || strcasecmp(ext, "otf") == 0 ||
           strcasecmp(ext, "ttc") == 0 || strcasecmp(ext, "woff") == 0 ||
           strcasecmp(ext, "woff2") == 0;
}

static bool font_path_allowed(const char *path)
{
    if (!path || path[0] == '\0') {
        return true;
    }
    const char *prefix = BOARD_SD_FONT_DIR "/";
    size_t prefix_len = strlen(prefix);
    return strncmp(path, prefix, prefix_len) == 0 && !strstr(path, "..") &&
           !strchr(path + prefix_len, '/') && font_type_allowed(path);
}

static void sanitize_upload_name(const char *name, const char *fallback, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    const char *src = name && name[0] ? name : fallback;
    size_t w = 0;
    for (size_t i = 0; src[i] && w + 1 < out_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c < 32 || c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            if (w > 0 && out[w - 1] != '_') {
                out[w++] = '_';
            }
            continue;
        }
        out[w++] = (char)c;
    }
    while (w > 0 && (out[w - 1] == ' ' || out[w - 1] == '.')) {
        w--;
    }
    if (w == 0) {
        snprintf(out, out_len, "%s", fallback);
    } else {
        out[w] = '\0';
    }
}

static void add_sd_status_to_json(cJSON *root)
{
    storage_sd_status_t sd = storage_sd_get_status();
    cJSON_AddBoolToObject(root, "mounted", sd.mounted);
    cJSON_AddNumberToObject(root, "card_size_mb", (double)(sd.card_size_bytes / 1024 / 1024));
    cJSON_AddNumberToObject(root, "total_mb", (double)(sd.total_bytes / 1024 / 1024));
    cJSON_AddNumberToObject(root, "free_mb", (double)(sd.free_bytes / 1024 / 1024));
    cJSON_AddStringToObject(root, "mount_point", BOARD_SD_MOUNT_POINT);
    cJSON_AddStringToObject(root, "cache_dir", BOARD_SD_CACHE_DIR);
    cJSON_AddStringToObject(root, "partdb_cache_dir", BOARD_SD_PARTDB_CACHE_DIR);
    cJSON_AddStringToObject(root, "image_cache_dir", BOARD_SD_IMAGE_CACHE_DIR);
    cJSON_AddStringToObject(root, "video_cache_dir", BOARD_SD_VIDEO_CACHE_DIR);
}

static void add_hw_item(cJSON *parent, const char *key, bool diag_started, esp_err_t err, bool ready)
{
    cJSON *item = cJSON_AddObjectToObject(parent, key);
    cJSON_AddBoolToObject(item, "ready", ready);
    cJSON_AddBoolToObject(item, "ok", diag_started ? err == ESP_OK : ready);
    cJSON_AddStringToObject(item, "err", diag_started ? esp_err_to_name(err) : "NOT_RUN");
}

static bool json_bool_value(cJSON *root, const char *key, bool fallback)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!item) {
        return fallback;
    }
    return cJSON_IsTrue(item);
}

static bool json_string_copy(cJSON *root, const char *key, char *out, size_t out_len)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsString(item) || !item->valuestring || !out || out_len == 0) {
        return false;
    }
    snprintf(out, out_len, "%s", item->valuestring);
    return true;
}

static bool parse_positive_int(const char *s, int *out)
{
    if (!s || !isdigit((unsigned char)*s) || !out) {
        return false;
    }
    long value = 0;
    while (isdigit((unsigned char)*s)) {
        value = value * 10 + (*s - '0');
        if (value > 99999999) {
            return false;
        }
        s++;
    }
    if (value <= 0) {
        return false;
    }
    *out = (int)value;
    return true;
}

static bool parse_barcode_int(const char *s, int *out)
{
    if (!s || !isdigit((unsigned char)*s) || !out) {
        return false;
    }
    long value = 0;
    while (isdigit((unsigned char)*s)) {
        value = value * 10 + (*s - '0');
        if (value > 99999999) {
            return false;
        }
        s++;
    }
    while (isspace((unsigned char)*s)) {
        s++;
    }
    if (*s != '\0' || value <= 0) {
        return false;
    }
    *out = (int)value;
    return true;
}

static bool parse_id_after_marker(const char *text, const char *marker, int *id)
{
    const char *p = text && marker ? strstr(text, marker) : NULL;
    if (!p) {
        return false;
    }
    p += strlen(marker);
    return parse_positive_int(p, id);
}

static bool target_set(partdb_barcode_target_t *target, char prefix, int id)
{
    if (!target || id <= 0) {
        return false;
    }
    const char *type = NULL;
    const char *api = NULL;
    const char *scan = NULL;
    switch (prefix) {
    case 'P':
        type = "part";
        api = "parts";
        scan = "part";
        break;
    case 'L':
        type = "lot";
        api = "part_lots";
        scan = "lot";
        break;
    case 'S':
        type = "location";
        api = "storage_locations";
        scan = "location";
        break;
    default:
        return false;
    }
    memset(target, 0, sizeof(*target));
    target->prefix = prefix;
    target->id = id;
    snprintf(target->type, sizeof(target->type), "%s", type);
    snprintf(target->barcode, sizeof(target->barcode), "%c%04d", prefix, id);
    snprintf(target->api_path, sizeof(target->api_path), "/api/%s/%d.jsonld", api, id);
    snprintf(target->scan_path, sizeof(target->scan_path), "/scan/%s/%d", scan, id);
    return true;
}

static bool target_from_barcode(const char *barcode, partdb_barcode_target_t *target)
{
    if (!barcode || !target) {
        return false;
    }
    while (isspace((unsigned char)*barcode)) {
        barcode++;
    }
    int id = 0;
    if (parse_id_after_marker(barcode, "/scan/part/", &id)) {
        return target_set(target, 'P', id);
    }
    if (parse_id_after_marker(barcode, "/scan/lot/", &id)) {
        return target_set(target, 'L', id);
    }
    if (parse_id_after_marker(barcode, "/scan/location/", &id)) {
        return target_set(target, 'S', id);
    }
    char prefix = (char)toupper((unsigned char)barcode[0]);
    if ((prefix == 'P' || prefix == 'L' || prefix == 'S') && parse_barcode_int(barcode + 1, &id)) {
        return target_set(target, prefix, id);
    }
    return false;
}

static bool target_from_partdb_path(const char *path, partdb_barcode_target_t *target)
{
    if (!path || !target || strstr(path, "://")) {
        return false;
    }
    int id = 0;
    if (parse_id_after_marker(path, "/parts/", &id)) {
        return target_set(target, 'P', id);
    }
    if (parse_id_after_marker(path, "/part_lots/", &id)) {
        return target_set(target, 'L', id);
    }
    if (parse_id_after_marker(path, "/storage_locations/", &id)) {
        return target_set(target, 'S', id);
    }
    return false;
}

static bool url_encode_component(const char *in, char *out, size_t out_len)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!in || !out || out_len == 0) {
        return false;
    }
    size_t w = 0;
    for (size_t i = 0; in[i]; i++) {
        unsigned char c = (unsigned char)in[i];
        bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                     c == '.' || c == '~';
        if (plain) {
            if (w + 1 >= out_len) {
                return false;
            }
            out[w++] = (char)c;
        } else {
            if (w + 3 >= out_len) {
                return false;
            }
            out[w++] = '%';
            out[w++] = hex[c >> 4];
            out[w++] = hex[c & 0x0f];
        }
    }
    out[w] = '\0';
    return true;
}

static bool target_from_user_barcode_lookup(cJSON *root, const char *barcode,
                                            partdb_barcode_target_t *target)
{
    cJSON *lookup = root ? cJSON_AddObjectToObject(root, "user_barcode_lookup") : NULL;
    if (lookup) {
        cJSON_AddStringToObject(lookup, "barcode", barcode ? barcode : "");
    }
    if (!barcode || !barcode[0] || !target) {
        if (lookup) {
            cJSON_AddBoolToObject(lookup, "ok", false);
            cJSON_AddStringToObject(lookup, "err", esp_err_to_name(ESP_ERR_INVALID_ARG));
        }
        return false;
    }

    char encoded[160];
    if (!url_encode_component(barcode, encoded, sizeof(encoded))) {
        if (lookup) {
            cJSON_AddBoolToObject(lookup, "ok", false);
            cJSON_AddStringToObject(lookup, "err", esp_err_to_name(ESP_ERR_INVALID_SIZE));
        }
        return false;
    }

    char path[256];
    snprintf(path, sizeof(path),
             "/api/part_lots.jsonld?user_barcode=%s&itemsPerPage=1&"
             "properties[]=id&properties[]=user_barcode",
             encoded);
    char *body = malloc(PARTDB_RESPONSE_MAX);
    if (!body) {
        if (lookup) {
            cJSON_AddBoolToObject(lookup, "ok", false);
            cJSON_AddStringToObject(lookup, "err", esp_err_to_name(ESP_ERR_NO_MEM));
        }
        return false;
    }

    int http_status = 0;
    esp_err_t err = partdb_client_get_json(path, body, PARTDB_RESPONSE_MAX, &http_status);
    bool found = false;
    if (lookup) {
        cJSON_AddStringToObject(lookup, "path", path);
        cJSON_AddStringToObject(lookup, "err", esp_err_to_name(err));
        cJSON_AddNumberToObject(lookup, "http_status", http_status);
        cJSON_AddStringToObject(lookup, "cache_source", partdb_client_last_source());
        cJSON_AddBoolToObject(lookup, "cache_hit", partdb_client_get_status().last_cache_hit);
        cJSON_AddNumberToObject(lookup, "body_len", (double)strlen(body));
    }

    cJSON *parsed = err == ESP_OK ? cJSON_Parse(body) : NULL;
    cJSON *members = parsed ? cJSON_GetObjectItem(parsed, "hydra:member") : NULL;
    cJSON *first = cJSON_IsArray(members) ? cJSON_GetArrayItem(members, 0) : NULL;
    cJSON *id = cJSON_IsObject(first) ? cJSON_GetObjectItem(first, "id") : NULL;
    int lot_id = 0;
    if (cJSON_IsNumber(id)) {
        lot_id = id->valueint;
    } else {
        cJSON *iri = cJSON_IsObject(first) ? cJSON_GetObjectItem(first, "@id") : NULL;
        if (cJSON_IsString(iri)) {
            (void)parse_id_after_marker(iri->valuestring, "/part_lots/", &lot_id);
        }
    }
    if (lot_id > 0) {
        found = target_set(target, 'L', lot_id);
    }
    if (lookup) {
        cJSON_AddBoolToObject(lookup, "ok", found);
        cJSON_AddBoolToObject(lookup, "matched", found);
        cJSON_AddNumberToObject(lookup, "matched_id", lot_id);
    }
    if (parsed) {
        cJSON_Delete(parsed);
    }
    free(body);
    return found;
}

static bool target_from_ipn_lookup(cJSON *root, const char *ipn,
                                   partdb_barcode_target_t *target)
{
    cJSON *lookup = root ? cJSON_AddObjectToObject(root, "ipn_lookup") : NULL;
    if (lookup) {
        cJSON_AddStringToObject(lookup, "ipn", ipn ? ipn : "");
    }
    if (!ipn || !ipn[0] || !target) {
        if (lookup) {
            cJSON_AddBoolToObject(lookup, "ok", false);
            cJSON_AddStringToObject(lookup, "err", esp_err_to_name(ESP_ERR_INVALID_ARG));
        }
        return false;
    }

    char encoded[320];
    if (!url_encode_component(ipn, encoded, sizeof(encoded))) {
        if (lookup) {
            cJSON_AddBoolToObject(lookup, "ok", false);
            cJSON_AddStringToObject(lookup, "err", esp_err_to_name(ESP_ERR_INVALID_SIZE));
        }
        return false;
    }

    char path[448];
    int path_written = snprintf(path, sizeof(path),
                                "/api/parts.jsonld?ipn=%s&itemsPerPage=1&"
                                "properties[]=id&properties[]=ipn&properties[]=name",
                                encoded);
    if (path_written < 0 || path_written >= (int)sizeof(path)) {
        if (lookup) {
            cJSON_AddBoolToObject(lookup, "ok", false);
            cJSON_AddStringToObject(lookup, "err", esp_err_to_name(ESP_ERR_INVALID_SIZE));
        }
        return false;
    }
    char *body = malloc(PARTDB_RESPONSE_MAX);
    if (!body) {
        if (lookup) {
            cJSON_AddBoolToObject(lookup, "ok", false);
            cJSON_AddStringToObject(lookup, "err", esp_err_to_name(ESP_ERR_NO_MEM));
        }
        return false;
    }

    int http_status = 0;
    esp_err_t err = partdb_client_get_json(path, body, PARTDB_RESPONSE_MAX, &http_status);
    bool found = false;
    if (lookup) {
        cJSON_AddStringToObject(lookup, "path", path);
        cJSON_AddStringToObject(lookup, "err", esp_err_to_name(err));
        cJSON_AddNumberToObject(lookup, "http_status", http_status);
        cJSON_AddStringToObject(lookup, "cache_source", partdb_client_last_source());
        cJSON_AddBoolToObject(lookup, "cache_hit", partdb_client_get_status().last_cache_hit);
        cJSON_AddNumberToObject(lookup, "body_len", (double)strlen(body));
    }

    cJSON *parsed = err == ESP_OK ? cJSON_Parse(body) : NULL;
    cJSON *members = parsed ? cJSON_GetObjectItem(parsed, "hydra:member") : NULL;
    cJSON *first = cJSON_IsArray(members) ? cJSON_GetArrayItem(members, 0) : NULL;
    cJSON *id = cJSON_IsObject(first) ? cJSON_GetObjectItem(first, "id") : NULL;
    int part_id = 0;
    if (cJSON_IsNumber(id)) {
        part_id = id->valueint;
    } else {
        cJSON *iri = cJSON_IsObject(first) ? cJSON_GetObjectItem(first, "@id") : NULL;
        if (cJSON_IsString(iri)) {
            (void)parse_id_after_marker(iri->valuestring, "/parts/", &part_id);
        }
    }
    if (part_id > 0) {
        found = target_set(target, 'P', part_id);
    }
    if (lookup) {
        cJSON_AddBoolToObject(lookup, "ok", found);
        cJSON_AddBoolToObject(lookup, "matched", found);
        cJSON_AddNumberToObject(lookup, "matched_id", part_id);
    }
    if (parsed) {
        cJSON_Delete(parsed);
    }
    free(body);
    return found;
}

static esp_err_t target_from_json(cJSON *req_json, partdb_barcode_target_t *target)
{
    if (!req_json || !target) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *part_id = cJSON_GetObjectItem(req_json, "part_id");
    if (cJSON_IsNumber(part_id) && target_set(target, 'P', part_id->valueint)) {
        return ESP_OK;
    }
    char value[128] = {0};
    if (json_string_copy(req_json, "barcode", value, sizeof(value)) &&
        target_from_barcode(value, target)) {
        return ESP_OK;
    }
    if ((json_string_copy(req_json, "partdb_path", value, sizeof(value)) ||
         json_string_copy(req_json, "path", value, sizeof(value))) &&
        target_from_partdb_path(value, target)) {
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

static void add_target_json(cJSON *root, const partdb_barcode_target_t *target)
{
    cJSON *obj = cJSON_AddObjectToObject(root, "target");
    cJSON_AddStringToObject(obj, "type", target ? target->type : "");
    cJSON_AddNumberToObject(obj, "id", target ? target->id : 0);
    cJSON_AddStringToObject(obj, "barcode", target ? target->barcode : "");
    cJSON_AddStringToObject(obj, "api_path", target ? target->api_path : "");
    cJSON_AddStringToObject(obj, "scan_path", target ? target->scan_path : "");
}

static void partdb_lookup_path_for_target(const partdb_barcode_target_t *target, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!target) {
        return;
    }
    if (target->prefix == 'P') {
        snprintf(out, out_len,
                 "/api/parts/%d.jsonld?properties[]=id&properties[]=name&properties[]=ipn&"
                 "properties[]=description&properties[category][]=name",
                 target->id);
        return;
    }
    if (target->prefix == 'L') {
        snprintf(out, out_len,
                 "/api/part_lots/%d.jsonld?properties[]=id&properties[]=description&"
                 "properties[]=user_barcode&properties[part][]=id&properties[part][]=name&"
                 "properties[storage_location][]=id&properties[storage_location][]=name",
                 target->id);
        return;
    }
    snprintf(out, out_len, "%s", target->api_path);
}

static bool add_partdb_lookup_json(cJSON *root, const partdb_barcode_target_t *target)
{
    cJSON *lookup = cJSON_AddObjectToObject(root, "partdb_lookup");
    if (!target) {
        cJSON_AddBoolToObject(lookup, "ok", false);
        cJSON_AddStringToObject(lookup, "err", esp_err_to_name(ESP_ERR_INVALID_ARG));
        return false;
    }

    char *body = malloc(PARTDB_RESPONSE_MAX);
    if (!body) {
        cJSON_AddBoolToObject(lookup, "ok", false);
        cJSON_AddStringToObject(lookup, "err", esp_err_to_name(ESP_ERR_NO_MEM));
        return false;
    }
    char lookup_path[384] = {0};
    partdb_lookup_path_for_target(target, lookup_path, sizeof(lookup_path));
    int http_status = 0;
    esp_err_t err = partdb_client_get_json(lookup_path, body, PARTDB_RESPONSE_MAX, &http_status);
    bool ok = err == ESP_OK && http_status >= 200 && http_status < 300;
    cJSON_AddBoolToObject(lookup, "ok", ok);
    cJSON_AddStringToObject(lookup, "err", esp_err_to_name(err));
    cJSON_AddStringToObject(lookup, "path", lookup_path);
    cJSON_AddNumberToObject(lookup, "http_status", http_status);
    cJSON_AddStringToObject(lookup, "cache_source", partdb_client_last_source());
    cJSON_AddBoolToObject(lookup, "cache_hit", partdb_client_get_status().last_cache_hit);
    cJSON_AddNumberToObject(lookup, "body_len", (double)strlen(body));

    cJSON *parsed = cJSON_Parse(body);
    cJSON_AddBoolToObject(lookup, "json_parsed", parsed != NULL);
    if (parsed) {
        cJSON *summary = cJSON_AddObjectToObject(lookup, "summary");
        cJSON *id = cJSON_GetObjectItem(parsed, "id");
        cJSON *name = cJSON_GetObjectItem(parsed, "name");
        cJSON *ipn = cJSON_GetObjectItem(parsed, "ipn");
        cJSON *description = cJSON_GetObjectItem(parsed, "description");
        cJSON *user_barcode = cJSON_GetObjectItem(parsed, "user_barcode");
        if (cJSON_IsNumber(id)) {
            cJSON_AddNumberToObject(summary, "id", id->valueint);
        }
        if (cJSON_IsString(name)) {
            cJSON_AddStringToObject(summary, "name", name->valuestring);
        }
        if (cJSON_IsString(ipn)) {
            cJSON_AddStringToObject(summary, "ipn", ipn->valuestring);
        }
        if (cJSON_IsString(description)) {
            cJSON_AddStringToObject(summary, "description", description->valuestring);
        }
        if (cJSON_IsString(user_barcode)) {
            cJSON_AddStringToObject(summary, "user_barcode", user_barcode->valuestring);
        }
        cJSON *category = cJSON_GetObjectItem(parsed, "category");
        cJSON *cat_name = cJSON_IsObject(category) ? cJSON_GetObjectItem(category, "name") : NULL;
        if (cJSON_IsString(cat_name)) {
            cJSON_AddStringToObject(summary, "category", cat_name->valuestring);
        }
        cJSON_Delete(parsed);
    }
    free(body);
    return ok;
}

static esp_err_t ensure_partdb_barcode_json(cJSON *root, const partdb_barcode_target_t *target,
                                            bool commit, bool force)
{
    cJSON *obj = cJSON_AddObjectToObject(root, "partdb_barcode");
    if (!target) {
        cJSON_AddBoolToObject(obj, "ok", false);
        cJSON_AddStringToObject(obj, "err", esp_err_to_name(ESP_ERR_INVALID_ARG));
        return ESP_ERR_INVALID_ARG;
    }

    cJSON_AddStringToObject(obj, "barcode", target->barcode);
    cJSON_AddStringToObject(obj, "target_type", target->type);
    cJSON_AddBoolToObject(obj, "write_attempted", false);
    cJSON_AddBoolToObject(obj, "created", false);
    cJSON_AddBoolToObject(obj, "updated", false);

    if (target->prefix != 'L') {
        cJSON_AddBoolToObject(obj, "ok", true);
        cJSON_AddStringToObject(obj, "strategy", "derived_internal");
        cJSON_AddBoolToObject(obj, "db_field_required", false);
        cJSON_AddStringToObject(obj, "err", esp_err_to_name(ESP_OK));
        return ESP_OK;
    }

    cJSON_AddStringToObject(obj, "strategy", "part_lot_user_barcode");
    cJSON_AddBoolToObject(obj, "db_field_required", true);

    char get_path[160];
    snprintf(get_path, sizeof(get_path),
             "/api/part_lots/%d.jsonld?properties[]=id&properties[]=user_barcode",
             target->id);
    char *body = malloc(PARTDB_RESPONSE_MAX);
    if (!body) {
        cJSON_AddBoolToObject(obj, "ok", false);
        cJSON_AddStringToObject(obj, "err", esp_err_to_name(ESP_ERR_NO_MEM));
        return ESP_ERR_NO_MEM;
    }

    int http_status = 0;
    esp_err_t err = partdb_client_get_json(get_path, body, PARTDB_RESPONSE_MAX, &http_status);
    cJSON_AddStringToObject(obj, "lookup_path", get_path);
    cJSON_AddNumberToObject(obj, "lookup_http_status", http_status);
    cJSON_AddStringToObject(obj, "lookup_source", partdb_client_last_source());
    if (err != ESP_OK || http_status < 200 || http_status >= 300) {
        cJSON_AddBoolToObject(obj, "ok", false);
        cJSON_AddStringToObject(obj, "err", esp_err_to_name(err == ESP_OK ? ESP_ERR_NOT_FOUND : err));
        free(body);
        return err == ESP_OK ? ESP_ERR_NOT_FOUND : err;
    }

    cJSON *parsed = cJSON_Parse(body);
    cJSON *user_barcode = parsed ? cJSON_GetObjectItem(parsed, "user_barcode") : NULL;
    const char *existing = cJSON_IsString(user_barcode) && user_barcode->valuestring ? user_barcode->valuestring : "";
    cJSON_AddStringToObject(obj, "existing", existing);
    bool missing = existing[0] == '\0';
    bool differs = !missing && strcmp(existing, target->barcode) != 0;
    cJSON_AddBoolToObject(obj, "missing", missing);
    cJSON_AddBoolToObject(obj, "differs", differs);

    if (parsed) {
        cJSON_Delete(parsed);
    }

    bool should_patch = missing || (force && differs);
    cJSON_AddBoolToObject(obj, "would_write", should_patch);
    if (!should_patch) {
        cJSON_AddBoolToObject(obj, "ok", true);
        cJSON_AddStringToObject(obj, "err", esp_err_to_name(ESP_OK));
        free(body);
        return ESP_OK;
    }
    if (!commit) {
        cJSON_AddBoolToObject(obj, "ok", true);
        cJSON_AddBoolToObject(obj, "dry_run", true);
        cJSON_AddStringToObject(obj, "err", esp_err_to_name(ESP_OK));
        free(body);
        return ESP_OK;
    }

    char patch_path[96];
    char patch_body[192];
    snprintf(patch_path, sizeof(patch_path), "/api/part_lots/%d.jsonld", target->id);
    int patch_written = snprintf(patch_body, sizeof(patch_body), "{\"user_barcode\":\"%s\"}", target->barcode);
    if (patch_written < 0 || patch_written >= (int)sizeof(patch_body)) {
        cJSON_AddBoolToObject(obj, "ok", false);
        cJSON_AddStringToObject(obj, "err", esp_err_to_name(ESP_ERR_INVALID_SIZE));
        free(body);
        return ESP_ERR_INVALID_SIZE;
    }
    cJSON_ReplaceItemInObject(obj, "write_attempted", cJSON_CreateBool(true));

    http_status = 0;
    err = partdb_client_patch_json(patch_path, patch_body, body, PARTDB_RESPONSE_MAX, &http_status);
    bool ok = err == ESP_OK && http_status >= 200 && http_status < 300;
    cJSON_AddStringToObject(obj, "patch_path", patch_path);
    cJSON_AddNumberToObject(obj, "patch_http_status", http_status);
    cJSON_ReplaceItemInObject(obj, "created", cJSON_CreateBool(ok && missing));
    cJSON_ReplaceItemInObject(obj, "updated", cJSON_CreateBool(ok && !missing));
    cJSON_AddBoolToObject(obj, "ok", ok);
    cJSON_AddStringToObject(obj, "err", esp_err_to_name(ok ? ESP_OK : (err == ESP_OK ? ESP_FAIL : err)));
    free(body);
    return ok ? ESP_OK : (err == ESP_OK ? ESP_FAIL : err);
}

static esp_err_t partdb_ipn_for_target_json(cJSON *root, const partdb_barcode_target_t *target,
                                            char *out, size_t out_len)
{
    cJSON *obj = cJSON_AddObjectToObject(root, "ipn_payload_lookup");
    if (!target || target->prefix != 'P' || !out || out_len == 0) {
        if (out && out_len > 0) {
            out[0] = '\0';
        }
        cJSON_AddBoolToObject(obj, "ok", false);
        cJSON_AddStringToObject(obj, "err", esp_err_to_name(ESP_ERR_INVALID_ARG));
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    char path[160];
    snprintf(path, sizeof(path), "/api/parts/%d.jsonld?properties[]=id&properties[]=ipn", target->id);
    char *body = malloc(PARTDB_RESPONSE_MAX);
    if (!body) {
        cJSON_AddBoolToObject(obj, "ok", false);
        cJSON_AddStringToObject(obj, "err", esp_err_to_name(ESP_ERR_NO_MEM));
        return ESP_ERR_NO_MEM;
    }

    int http_status = 0;
    esp_err_t err = partdb_client_get_json(path, body, PARTDB_RESPONSE_MAX, &http_status);
    bool ok = false;
    cJSON_AddStringToObject(obj, "path", path);
    cJSON_AddNumberToObject(obj, "http_status", http_status);
    cJSON_AddStringToObject(obj, "cache_source", partdb_client_last_source());

    cJSON *parsed = err == ESP_OK ? cJSON_Parse(body) : NULL;
    cJSON *ipn = parsed ? cJSON_GetObjectItem(parsed, "ipn") : NULL;
    if (cJSON_IsString(ipn) && ipn->valuestring && ipn->valuestring[0] &&
        strlen(ipn->valuestring) < out_len) {
        snprintf(out, out_len, "%s", ipn->valuestring);
        ok = true;
    } else if (err == ESP_OK) {
        err = ESP_ERR_NOT_FOUND;
    }
    cJSON_AddBoolToObject(obj, "ok", ok);
    cJSON_AddStringToObject(obj, "err", esp_err_to_name(err));
    cJSON_AddStringToObject(obj, "ipn", out);
    if (parsed) {
        cJSON_Delete(parsed);
    }
    free(body);
    return ok ? ESP_OK : err;
}

static const asset_kind_t *asset_kind_for(const char *kind)
{
    if (!kind) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(ASSET_KINDS) / sizeof(ASSET_KINDS[0]); i++) {
        if (strcmp(kind, ASSET_KINDS[i].kind) == 0) {
            return &ASSET_KINDS[i];
        }
    }
    return NULL;
}

static bool path_in_asset_dir(const char *path, const asset_kind_t *kind)
{
    if (!path || !kind || !sd_relative_path_is_safe(path)) {
        return false;
    }
    size_t dir_len = strlen(kind->rel_dir);
    return strncmp(path, kind->rel_dir, dir_len) == 0 &&
           path[dir_len] == '/' && strchr(path + dir_len + 1, '/') == NULL;
}

static const asset_kind_t *asset_kind_for_path(const char *path)
{
    for (size_t i = 0; i < sizeof(ASSET_KINDS) / sizeof(ASSET_KINDS[0]); i++) {
        if (path_in_asset_dir(path, &ASSET_KINDS[i])) {
            return &ASSET_KINDS[i];
        }
    }
    return NULL;
}

static char *selected_asset_path_mut(const asset_kind_t *kind)
{
    if (!s_cfg || !kind) {
        return NULL;
    }
    if (strcmp(kind->kind, "screen_bg") == 0) {
        return s_cfg->screen_bg_path;
    }
    if (strcmp(kind->kind, "boot_anim") == 0) {
        return s_cfg->boot_anim_path;
    }
    if (strcmp(kind->kind, "lock_bg") == 0) {
        return s_cfg->lock_bg_path;
    }
    return NULL;
}

static const char *selected_asset_path_const(const asset_kind_t *kind)
{
    const char *path = selected_asset_path_mut(kind);
    return path ? path : "";
}

static bool asset_type_allowed(const asset_kind_t *kind, const char *name)
{
    if (!kind || strcmp(file_type_for_name(name), "image") != 0) {
        return false;
    }
    if (!kind->animated) {
        return true;
    }
    const char *ext = name ? strrchr(name, '.') : NULL;
    if (!ext) {
        return false;
    }
    ext++;
    return strcasecmp(ext, "gif") == 0 || strcasecmp(ext, "webp") == 0;
}

static bool selected_asset_path_allowed(const asset_kind_t *kind, const char *path)
{
    if (!path || path[0] == '\0') {
        return true;
    }
    return kind && path_in_asset_dir(path, kind) && asset_type_allowed(kind, path);
}

static const char *asset_upload_ext(const asset_kind_t *kind, const char *name)
{
    if (!kind || !kind->animated) {
        return ".jpg";
    }
    const char *ext = name ? strrchr(name, '.') : NULL;
    if (ext && strcasecmp(ext, ".webp") == 0) {
        return ".webp";
    }
    return ".gif";
}

static void sanitize_asset_name(const char *name, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    const char *src = name && name[0] ? name : "asset";
    size_t w = 0;
    for (size_t i = 0; src[i] && w + 1 < out_len; i++) {
        char c = src[i];
        if (c == '.') {
            break;
        }
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-') {
            out[w++] = c;
        } else if (w > 0 && out[w - 1] != '_') {
            out[w++] = '_';
        }
    }
    if (w == 0) {
        snprintf(out, out_len, "asset");
    } else {
        out[w] = '\0';
    }
}

static int scan_asset_entries(const asset_kind_t *kind, asset_entry_t *entries,
                              int max_entries, int *oldest_index)
{
    if (oldest_index) {
        *oldest_index = -1;
    }
    if (!kind || storage_sd_prepare_paths() != ESP_OK) {
        return 0;
    }

    char abs_dir[HTTP_SD_PATH_MAX];
    if (make_sd_path(kind->rel_dir, abs_dir, sizeof(abs_dir)) != ESP_OK) {
        return 0;
    }
    DIR *dir = opendir(abs_dir);
    if (!dir) {
        return 0;
    }

    struct dirent *ent = NULL;
    int count = 0;
    long oldest_mtime = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0 ||
            !asset_type_allowed(kind, ent->d_name)) {
            continue;
        }
        char rel_path[HTTP_SD_PATH_MAX];
        int written = snprintf(rel_path, sizeof(rel_path), "%s/%s", kind->rel_dir, ent->d_name);
        if (written < 0 || written >= (int)sizeof(rel_path)) {
            continue;
        }
        char abs_path[HTTP_SD_PATH_MAX];
        if (make_sd_path(rel_path, abs_path, sizeof(abs_path)) != ESP_OK) {
            continue;
        }
        struct stat st;
        if (stat(abs_path, &st) != 0 || S_ISDIR(st.st_mode)) {
            continue;
        }

        if (entries && count < max_entries) {
            app_config_copy_string(entries[count].name, sizeof(entries[count].name), ent->d_name);
            snprintf(entries[count].path, sizeof(entries[count].path), "%s", rel_path);
            snprintf(entries[count].type, sizeof(entries[count].type), "%s", file_type_for_name(ent->d_name));
            entries[count].size = (size_t)st.st_size;
            entries[count].mtime = (long)st.st_mtime;
        }
        if (oldest_index && count < max_entries &&
            (*oldest_index < 0 || (long)st.st_mtime < oldest_mtime)) {
            *oldest_index = count;
            oldest_mtime = (long)st.st_mtime;
        }
        count++;
    }
    closedir(dir);
    return count;
}

static void add_asset_entry_json(cJSON *parent, const char *key, const asset_entry_t *entry,
                                 const char *selected_path)
{
    if (!parent || !entry) {
        return;
    }
    cJSON *obj = key ? cJSON_AddObjectToObject(parent, key) : cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "name", entry->name);
    cJSON_AddStringToObject(obj, "path", entry->path);
    cJSON_AddStringToObject(obj, "type", entry->type);
    cJSON_AddNumberToObject(obj, "size", (double)entry->size);
    cJSON_AddNumberToObject(obj, "mtime", (double)entry->mtime);
    cJSON_AddBoolToObject(obj, "deletable", true);
    cJSON_AddBoolToObject(obj, "selected",
                          selected_path && strcmp(entry->path, selected_path) == 0);
    if (!key) {
        cJSON_AddItemToArray(parent, obj);
    }
}

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t config_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_name", s_cfg->device_name);
    cJSON_AddStringToObject(root, "wifi_ssid", s_cfg->wifi_ssid);
    cJSON_AddStringToObject(root, "wifi_pass", "");
    cJSON_AddBoolToObject(root, "wifi_pass_set", s_cfg->wifi_pass[0] != '\0');
    cJSON_AddStringToObject(root, "partdb_url", s_cfg->partdb_url);
    cJSON_AddStringToObject(root, "partdb_token", "");
    cJSON_AddStringToObject(root, "boot_image_path", s_cfg->boot_image_path);
    cJSON_AddStringToObject(root, "font_dir", s_cfg->font_dir);
    cJSON_AddStringToObject(root, "font_path", s_cfg->font_path);
    cJSON_AddStringToObject(root, "screen_bg_path", s_cfg->screen_bg_path);
    cJSON_AddStringToObject(root, "boot_anim_path", s_cfg->boot_anim_path);
    cJSON_AddStringToObject(root, "lock_bg_path", s_cfg->lock_bg_path);
    cJSON_AddStringToObject(root, "display_driver", s_cfg->display_driver);
    cJSON_AddStringToObject(root, "display_orientation", s_cfg->display_orientation);
    cJSON_AddBoolToObject(root, "display_flip", s_cfg->display_flip);
    cJSON_AddNumberToObject(root, "touch_rotation", s_cfg->touch_rotation);
    cJSON_AddBoolToObject(root, "touch_swap_xy", s_cfg->touch_swap_xy);
    cJSON_AddBoolToObject(root, "touch_flip_x", s_cfg->touch_flip_x);
    cJSON_AddBoolToObject(root, "touch_flip_y", s_cfg->touch_flip_y);
    cJSON_AddNumberToObject(root, "display_width", s_cfg->display_width);
    cJSON_AddNumberToObject(root, "display_height", s_cfg->display_height);
    cJSON_AddNumberToObject(root, "touch_raw_width", s_cfg->touch_raw_width);
    cJSON_AddNumberToObject(root, "touch_raw_height", s_cfg->touch_raw_height);
    cJSON_AddBoolToObject(root, "ap_enabled", s_cfg->ap_enabled);
    cJSON_AddBoolToObject(root, "wifi_provisioned", s_cfg->wifi_provisioned);
    cJSON_AddNumberToObject(root, "display_brightness", s_cfg->display_brightness);
    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static void cfg_update_string(cJSON *root, const char *key, char *dst, unsigned dst_len, bool allow_empty)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsString(item)) {
        return;
    }
    if (!allow_empty && item->valuestring[0] == '\0') {
        return;
    }
    app_config_copy_string(dst, dst_len, item->valuestring);
}

static void cfg_update_bool(cJSON *root, const char *key, bool *dst)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (cJSON_IsBool(item)) {
        *dst = cJSON_IsTrue(item);
    }
}

static void cfg_update_u8(cJSON *root, const char *key, uint8_t *dst, uint8_t max_value)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsNumber(item)) {
        return;
    }
    int value = item->valueint;
    if (value < 0) {
        value = 0;
    } else if (value > max_value) {
        value = max_value;
    }
    *dst = (uint8_t)value;
}

static void cfg_update_u16(cJSON *root, const char *key, uint16_t *dst, uint16_t min_value, uint16_t max_value)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsNumber(item)) {
        return;
    }
    int value = item->valueint;
    if (value < min_value) {
        value = min_value;
    } else if (value > max_value) {
        value = max_value;
    }
    *dst = (uint16_t)value;
}

static bool json_has(cJSON *root, const char *key)
{
    return cJSON_GetObjectItem(root, key) != NULL;
}

static esp_err_t config_post(httpd_req_t *req)
{
    char body[1536];
    if (!read_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_OK;
    }

    bool wifi_changed = json_has(root, "wifi_ssid") || json_has(root, "wifi_pass") ||
                        json_has(root, "ap_enabled");
    bool partdb_changed = json_has(root, "partdb_url") || json_has(root, "partdb_token");
    bool font_changed = json_has(root, "font_path");
    bool brightness_changed = json_has(root, "display_brightness");
    bool display_cfg_changed = json_has(root, "display_driver") || json_has(root, "display_orientation") ||
                               json_has(root, "display_flip") || json_has(root, "display_width") ||
                               json_has(root, "display_height");
    bool touch_cfg_changed = json_has(root, "touch_rotation") || json_has(root, "touch_swap_xy") ||
                             json_has(root, "touch_flip_x") || json_has(root, "touch_flip_y") ||
                             json_has(root, "touch_raw_width") || json_has(root, "touch_raw_height");

    cfg_update_string(root, "wifi_ssid", s_cfg->wifi_ssid, sizeof(s_cfg->wifi_ssid), true);
    cfg_update_string(root, "device_name", s_cfg->device_name, sizeof(s_cfg->device_name), false);
    cfg_update_string(root, "wifi_pass", s_cfg->wifi_pass, sizeof(s_cfg->wifi_pass), false);
    cfg_update_string(root, "partdb_url", s_cfg->partdb_url, sizeof(s_cfg->partdb_url), true);
    cfg_update_string(root, "partdb_token", s_cfg->partdb_token, sizeof(s_cfg->partdb_token), false);
    app_config_copy_string(s_cfg->boot_image_path, sizeof(s_cfg->boot_image_path), BOARD_SD_BOOT_DIR "/boot.jpg");
    app_config_copy_string(s_cfg->font_dir, sizeof(s_cfg->font_dir), BOARD_SD_FONT_DIR);
    cfg_update_string(root, "font_path", s_cfg->font_path, sizeof(s_cfg->font_path), true);
    if (!font_path_allowed(s_cfg->font_path)) {
        s_cfg->font_path[0] = '\0';
    }
    cfg_update_string(root, "display_driver", s_cfg->display_driver, sizeof(s_cfg->display_driver), false);
    cfg_update_string(root, "display_orientation", s_cfg->display_orientation, sizeof(s_cfg->display_orientation), false);
    if (strcmp(s_cfg->display_orientation, "landscape") != 0) {
        app_config_copy_string(s_cfg->display_orientation, sizeof(s_cfg->display_orientation), "portrait");
    }
    cfg_update_bool(root, "ap_enabled", &s_cfg->ap_enabled);
    cfg_update_bool(root, "display_flip", &s_cfg->display_flip);
    cfg_update_bool(root, "touch_swap_xy", &s_cfg->touch_swap_xy);
    cfg_update_bool(root, "touch_flip_x", &s_cfg->touch_flip_x);
    cfg_update_bool(root, "touch_flip_y", &s_cfg->touch_flip_y);
    cfg_update_u8(root, "display_brightness", &s_cfg->display_brightness, 100);
    cfg_update_u8(root, "touch_rotation", &s_cfg->touch_rotation, 3);
    cfg_update_u16(root, "display_width", &s_cfg->display_width, 64, 1024);
    cfg_update_u16(root, "display_height", &s_cfg->display_height, 64, 1024);
    cfg_update_u16(root, "touch_raw_width", &s_cfg->touch_raw_width, 64, 1024);
    cfg_update_u16(root, "touch_raw_height", &s_cfg->touch_raw_height, 64, 1024);

    esp_err_t err = app_config_save(s_cfg);
    if (err == ESP_OK) {
        if (partdb_changed) {
            partdb_client_init(s_cfg);
        }
        if (font_changed) {
            ui_font_set_active_path(s_cfg->font_path);
            (void)device_ui_request_redraw();
        }
        if (display_cfg_changed) {
            (void)display_ili9488_configure(s_cfg->display_driver, s_cfg->display_width,
                                            s_cfg->display_height, s_cfg->display_orientation,
                                            s_cfg->display_flip);
            (void)device_ui_request_redraw();
        }
        if (touch_cfg_changed) {
            (void)device_ui_request_redraw();
        }
        if (brightness_changed) {
            display_ili9488_set_brightness(s_cfg->display_brightness);
            (void)device_ui_request_redraw();
        }
        if (wifi_changed) {
            wifi_portal_reconfigure(s_cfg);
        }
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
    cJSON_AddStringToObject(resp, "err", esp_err_to_name(err));
    esp_err_t send_err = send_json(req, resp);
    cJSON_Delete(resp);
    cJSON_Delete(root);
    return send_err;
}

static esp_err_t status_get(httpd_req_t *req)
{
    wifi_portal_status_t wifi = wifi_portal_get_status();
    storage_sd_status_t sd = storage_sd_get_status();
    partdb_client_status_t partdb = partdb_client_get_status();
    hardware_diag_status_t diag = hardware_diag_get_status();
    nfc_service_status_t nfc = nfc_service_get_status();
    qr_scanner_status_t qr = qr_scanner_get_status();
    button_input_status_t buttons = button_input_get_status();
    const esp_app_desc_t *app = esp_app_get_description();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_name", s_cfg->device_name);
    cJSON_AddStringToObject(root, "version", app ? app->version : "unknown");
    cJSON *about = cJSON_AddObjectToObject(root, "about");
    cJSON_AddStringToObject(about, "developer", BOARD_DEVELOPER_NAME);
    cJSON_AddStringToObject(about, "source_repo_url", BOARD_SOURCE_REPO_URL);
    cJSON_AddBoolToObject(about, "source_repo_reserved", BOARD_SOURCE_REPO_URL[0] == '\0');
    cJSON *jw = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddBoolToObject(jw, "ap_started", wifi.ap_started);
    cJSON_AddBoolToObject(jw, "ap_enabled", wifi.ap_enabled);
    cJSON_AddBoolToObject(jw, "wifi_provisioned", wifi.wifi_provisioned);
    cJSON_AddBoolToObject(jw, "wifi_pass_set", s_cfg->wifi_pass[0] != '\0');
    cJSON_AddBoolToObject(jw, "sta_connected", wifi.sta_connected);
    cJSON_AddStringToObject(jw, "ip", wifi.ip);
    cJSON_AddStringToObject(jw, "ssid", wifi.ssid);
    cJSON *jsd = cJSON_AddObjectToObject(root, "sd");
    cJSON_AddBoolToObject(jsd, "mounted", sd.mounted);
    cJSON_AddNumberToObject(jsd, "card_size_mb", (double)(sd.card_size_bytes / 1024 / 1024));
    cJSON_AddNumberToObject(jsd, "total_mb", (double)(sd.total_bytes / 1024 / 1024));
    cJSON_AddNumberToObject(jsd, "free_mb", (double)(sd.free_bytes / 1024 / 1024));
    cJSON_AddStringToObject(jsd, "mount_point", BOARD_SD_MOUNT_POINT);
    cJSON_AddStringToObject(jsd, "cache_dir", BOARD_SD_CACHE_DIR);
    cJSON_AddStringToObject(jsd, "partdb_cache_dir", BOARD_SD_PARTDB_CACHE_DIR);
    cJSON_AddStringToObject(jsd, "format", "FAT/FAT32");
    cJSON *jp = cJSON_AddObjectToObject(root, "partdb");
    cJSON_AddBoolToObject(jp, "configured", partdb.configured);
    cJSON_AddBoolToObject(jp, "last_ok", partdb.last_ok);
    cJSON_AddBoolToObject(jp, "last_cache_hit", partdb.last_cache_hit);
    cJSON_AddBoolToObject(jp, "last_cache_sd", partdb.last_cache_sd);
    cJSON_AddStringToObject(jp, "last_source", partdb_client_last_source());
    cJSON_AddNumberToObject(jp, "last_http_status", partdb.last_http_status);
    cJSON_AddNumberToObject(jp, "last_response_len", (double)partdb.last_response_len);
    cJSON *hw = cJSON_AddObjectToObject(root, "hardware");
    cJSON_AddBoolToObject(hw, "display_ready", display_ili9488_is_ready());
    cJSON_AddBoolToObject(hw, "display_awake", display_ili9488_is_awake());
    cJSON_AddNumberToObject(hw, "display_brightness", display_ili9488_get_brightness());
    cJSON_AddStringToObject(hw, "display_driver", display_ili9488_get_driver());
    cJSON_AddStringToObject(hw, "display_orientation", display_ili9488_get_orientation());
    cJSON_AddBoolToObject(hw, "display_flip", display_ili9488_get_flip());
    cJSON_AddNumberToObject(hw, "display_width", display_ili9488_get_width());
    cJSON_AddNumberToObject(hw, "display_height", display_ili9488_get_height());
    cJSON_AddNumberToObject(hw, "touch_rotation", s_cfg ? s_cfg->touch_rotation : 2);
    cJSON_AddBoolToObject(hw, "touch_swap_xy", s_cfg ? s_cfg->touch_swap_xy : false);
    cJSON_AddBoolToObject(hw, "touch_flip_x", s_cfg ? s_cfg->touch_flip_x : false);
    cJSON_AddBoolToObject(hw, "touch_flip_y", s_cfg ? s_cfg->touch_flip_y : false);
    cJSON_AddNumberToObject(hw, "touch_raw_width", s_cfg ? s_cfg->touch_raw_width : BOARD_TOUCH_RAW_WIDTH);
    cJSON_AddNumberToObject(hw, "touch_raw_height", s_cfg ? s_cfg->touch_raw_height : BOARD_TOUCH_RAW_HEIGHT);
    cJSON_AddBoolToObject(hw, "touch_ready", touch_ft6336_is_ready());
    cJSON_AddBoolToObject(hw, "nfc_ready", nfc_pn532_is_ready());
    cJSON_AddBoolToObject(hw, "camera_active", camera_mgr_is_active());
    cJSON *ji2c = cJSON_AddObjectToObject(hw, "i2c");
    cJSON_AddStringToObject(ji2c, "touch_backend", "hardware");
    cJSON_AddNumberToObject(ji2c, "touch_sda_gpio", BOARD_I2C_SDA_GPIO);
    cJSON_AddNumberToObject(ji2c, "touch_scl_gpio", BOARD_I2C_SCL_GPIO);
    cJSON_AddNumberToObject(ji2c, "touch_addr", BOARD_TOUCH_FT6336_ADDR);
    cJSON_AddStringToObject(ji2c, "nfc_backend", nfc_i2c_backend());
    cJSON_AddNumberToObject(ji2c, "nfc_sda_gpio", BOARD_NFC_SOFT_SDA_GPIO);
    cJSON_AddNumberToObject(ji2c, "nfc_scl_gpio", BOARD_NFC_SOFT_SCL_GPIO);
    cJSON_AddNumberToObject(ji2c, "nfc_addr", BOARD_NFC_PN532_ADDR);
    cJSON *jb = cJSON_AddObjectToObject(hw, "buttons");
    cJSON_AddBoolToObject(jb, "started", buttons.started);
    cJSON_AddNumberToObject(jb, "configured_count", buttons.configured_count);
    cJSON_AddBoolToObject(jb, "up_pressed", buttons.up_pressed);
    cJSON_AddBoolToObject(jb, "down_pressed", buttons.down_pressed);
    cJSON_AddBoolToObject(jb, "ok_pressed", buttons.ok_pressed);
    cJSON_AddBoolToObject(jb, "wake_pressed", buttons.wake_pressed);
    cJSON_AddStringToObject(jb, "last_event", buttons.last_event);
    cJSON_AddNumberToObject(jb, "event_count", buttons.event_count);
    device_ui_status_t ui = device_ui_get_status();
    cJSON *jui = cJSON_AddObjectToObject(hw, "ui");
    cJSON_AddBoolToObject(jui, "started", ui.started);
    cJSON_AddBoolToObject(jui, "awake", ui.awake);
    cJSON_AddNumberToObject(jui, "page", ui.page);
    cJSON_AddStringToObject(jui, "page_name", ui.page_name);
    cJSON_AddNumberToObject(jui, "redraw_count", ui.redraw_count);
    cJSON_AddNumberToObject(jui, "handled_button_count", ui.handled_button_count);
    cJSON_AddStringToObject(jui, "last_button_event", ui.last_button_event);
    cJSON_AddNumberToObject(jui, "touch_event_count", ui.touch_event_count);
    cJSON_AddStringToObject(jui, "last_touch_event", ui.last_touch_event);
    cJSON_AddBoolToObject(jui, "touch_range_valid", ui.touch_range_valid);
    cJSON_AddNumberToObject(jui, "last_touch_raw_x", ui.last_touch_raw_x);
    cJSON_AddNumberToObject(jui, "last_touch_raw_y", ui.last_touch_raw_y);
    cJSON_AddNumberToObject(jui, "last_touch_x", ui.last_touch_x);
    cJSON_AddNumberToObject(jui, "last_touch_y", ui.last_touch_y);
    cJSON_AddNumberToObject(jui, "touch_raw_min_x", ui.touch_raw_min_x);
    cJSON_AddNumberToObject(jui, "touch_raw_max_x", ui.touch_raw_max_x);
    cJSON_AddNumberToObject(jui, "touch_raw_min_y", ui.touch_raw_min_y);
    cJSON_AddNumberToObject(jui, "touch_raw_max_y", ui.touch_raw_max_y);
    cJSON_AddNumberToObject(jui, "touch_min_x", ui.touch_min_x);
    cJSON_AddNumberToObject(jui, "touch_max_x", ui.touch_max_x);
    cJSON_AddNumberToObject(jui, "touch_min_y", ui.touch_min_y);
    cJSON_AddNumberToObject(jui, "touch_max_y", ui.touch_max_y);
    cJSON *jnfc = cJSON_AddObjectToObject(hw, "nfc_service");
    cJSON_AddBoolToObject(jnfc, "started", nfc.started);
    cJSON_AddBoolToObject(jnfc, "ready", nfc.ready);
    cJSON_AddBoolToObject(jnfc, "paused_for_camera", nfc.paused_for_camera);
    cJSON_AddBoolToObject(jnfc, "paused_for_request", nfc.paused_for_request);
    cJSON_AddBoolToObject(jnfc, "tag_present", nfc.tag_present);
    cJSON_AddStringToObject(jnfc, "uid", nfc.uid);
    cJSON_AddStringToObject(jnfc, "text", nfc.text);
    cJSON_AddNumberToObject(jnfc, "last_seen_ms", nfc.last_seen_ms);
    cJSON_AddNumberToObject(jnfc, "read_count", nfc.read_count);
    cJSON_AddNumberToObject(jnfc, "text_read_count", nfc.text_read_count);
    cJSON_AddNumberToObject(jnfc, "error_count", nfc.error_count);
    cJSON_AddStringToObject(jnfc, "last_err", esp_err_to_name(nfc.last_err));
    cJSON *jqr = cJSON_AddObjectToObject(hw, "qr_scanner");
    cJSON_AddBoolToObject(jqr, "last_found", qr.last_found);
    cJSON_AddStringToObject(jqr, "last_text", qr.last_text);
    cJSON_AddNumberToObject(jqr, "last_code_count", qr.last_code_count);
    cJSON_AddNumberToObject(jqr, "last_width", qr.last_width);
    cJSON_AddNumberToObject(jqr, "last_height", qr.last_height);
    cJSON_AddNumberToObject(jqr, "last_elapsed_ms", qr.last_elapsed_ms);
    cJSON_AddNumberToObject(jqr, "scan_count", qr.scan_count);
    cJSON_AddNumberToObject(jqr, "found_count", qr.found_count);
    cJSON_AddStringToObject(jqr, "last_err", esp_err_to_name(qr.last_err));
    cJSON_AddStringToObject(jqr, "last_decode_error", qr.last_decode_error);
    cJSON *jd = cJSON_AddObjectToObject(hw, "diagnostics");
    cJSON_AddBoolToObject(jd, "started", diag.started);
    cJSON_AddBoolToObject(jd, "running", diag.running);
    cJSON_AddBoolToObject(jd, "finished", diag.finished);
    cJSON_AddNumberToObject(jd, "last_run_ms", diag.last_run_ms);
    add_hw_item(jd, "display", diag.started, diag.display_err, display_ili9488_is_ready());
    add_hw_item(jd, "touch", diag.started, diag.touch_err, touch_ft6336_is_ready());
    add_hw_item(jd, "nfc", diag.started, diag.nfc_err, nfc_pn532_is_ready());
    add_hw_item(jd, "tf", diag.started, diag.sd_err, sd.mounted);
    add_hw_item(jd, "camera", diag.started, diag.camera_err, camera_mgr_is_active());

    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t touch_get(httpd_req_t *req)
{
    if (!touch_ft6336_is_ready()) {
        (void)touch_ft6336_init();
    }
    touch_point_t point = {0};
    esp_err_t err = touch_ft6336_read(&point);
    uint16_t raw_x = point.x;
    uint16_t raw_y = point.y;
    touch_ft6336_transform_to_display(&point,
                                      s_cfg ? s_cfg->touch_swap_xy : false,
                                      s_cfg ? s_cfg->touch_raw_width : BOARD_TOUCH_RAW_WIDTH,
                                      s_cfg ? s_cfg->touch_raw_height : BOARD_TOUCH_RAW_HEIGHT,
                                      s_cfg ? s_cfg->touch_rotation : 2,
                                      s_cfg ? s_cfg->touch_flip_x : false,
                                      s_cfg ? s_cfg->touch_flip_y : false,
                                      display_ili9488_get_width(),
                                      display_ili9488_get_height());

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON_AddStringToObject(root, "err", esp_err_to_name(err));
    cJSON_AddBoolToObject(root, "ready", touch_ft6336_is_ready());
    cJSON_AddBoolToObject(root, "touched", point.touched);
    cJSON_AddNumberToObject(root, "points", point.points);
    cJSON_AddNumberToObject(root, "raw_x", raw_x);
    cJSON_AddNumberToObject(root, "raw_y", raw_y);
    cJSON_AddNumberToObject(root, "x", point.x);
    cJSON_AddNumberToObject(root, "y", point.y);
    cJSON_AddStringToObject(root, "display_orientation", display_ili9488_get_orientation());
    cJSON_AddBoolToObject(root, "display_flip", display_ili9488_get_flip());
    cJSON_AddNumberToObject(root, "display_width", display_ili9488_get_width());
    cJSON_AddNumberToObject(root, "display_height", display_ili9488_get_height());
    cJSON_AddNumberToObject(root, "touch_rotation", s_cfg ? s_cfg->touch_rotation : 2);
    cJSON_AddBoolToObject(root, "touch_swap_xy", s_cfg ? s_cfg->touch_swap_xy : false);
    cJSON_AddBoolToObject(root, "touch_flip_x", s_cfg ? s_cfg->touch_flip_x : false);
    cJSON_AddBoolToObject(root, "touch_flip_y", s_cfg ? s_cfg->touch_flip_y : false);
    cJSON_AddNumberToObject(root, "touch_raw_width", s_cfg ? s_cfg->touch_raw_width : BOARD_TOUCH_RAW_WIDTH);
    cJSON_AddNumberToObject(root, "touch_raw_height", s_cfg ? s_cfg->touch_raw_height : BOARD_TOUCH_RAW_HEIGHT);
    ESP_LOGI(TAG, "touch read err=%s ready=%d touched=%d points=%u raw=%u,%u mapped=%u,%u",
             esp_err_to_name(err), touch_ft6336_is_ready(), point.touched,
             point.points, raw_x, raw_y, point.x, point.y);
    esp_err_t send_err = send_json(req, root);
    cJSON_Delete(root);
    return send_err;
}

static esp_err_t display_test_get(httpd_req_t *req)
{
    (void)display_ili9488_configure(s_cfg->display_driver, s_cfg->display_width,
                                    s_cfg->display_height, s_cfg->display_orientation,
                                    s_cfg->display_flip);
    display_ili9488_set_brightness(s_cfg->display_brightness);
    esp_err_t err = display_ili9488_init();
    if (err == ESP_OK) {
        uint16_t width = display_ili9488_get_width();
        uint16_t height = display_ili9488_get_height();
        int band = height / 5;
        display_ili9488_fill_rect(0, 0, width, height, 0x0000);
        display_ili9488_fill_rect(0, 0, width, band, 0xF800);
        display_ili9488_fill_rect(0, band, width, band, 0x07E0);
        display_ili9488_fill_rect(0, band * 2, width, band, 0x001F);
        display_ili9488_fill_rect(0, band * 3, width, band, 0xFFE0);
        display_ili9488_fill_rect(0, band * 4, width, height - band * 4, 0xFFFF);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON_AddStringToObject(root, "err", esp_err_to_name(err));
    cJSON_AddBoolToObject(root, "ready", display_ili9488_is_ready());
    cJSON_AddStringToObject(root, "display_driver", display_ili9488_get_driver());
    cJSON_AddStringToObject(root, "display_orientation", display_ili9488_get_orientation());
    cJSON_AddBoolToObject(root, "display_flip", display_ili9488_get_flip());
    cJSON_AddNumberToObject(root, "display_width", display_ili9488_get_width());
    cJSON_AddNumberToObject(root, "display_height", display_ili9488_get_height());
    cJSON_AddStringToObject(root, "expect", "screen shows red/green/blue/yellow/white bands");
    esp_err_t send_err = send_json(req, root);
    cJSON_Delete(root);
    return send_err;
}

static esp_err_t display_brightness_post(httpd_req_t *req)
{
    char body[128];
    if (!read_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_OK;
    }

    cfg_update_u8(root, "display_brightness", &s_cfg->display_brightness, 100);
    esp_err_t err = app_config_save(s_cfg);
    if (err == ESP_OK) {
        err = display_ili9488_set_brightness(s_cfg->display_brightness);
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
    cJSON_AddStringToObject(resp, "err", esp_err_to_name(err));
    cJSON_AddNumberToObject(resp, "display_brightness", s_cfg->display_brightness);
    esp_err_t send_err = send_json(req, resp);
    cJSON_Delete(resp);
    cJSON_Delete(root);
    return send_err;
}

static esp_err_t display_power_post(httpd_req_t *req)
{
    char body[128];
    if (!read_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_OK;
    }

    bool awake = true;
    cJSON *item = cJSON_GetObjectItem(root, "awake");
    if (cJSON_IsBool(item)) {
        awake = cJSON_IsTrue(item);
    }
    esp_err_t err = display_ili9488_set_awake(awake);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
    cJSON_AddStringToObject(resp, "err", esp_err_to_name(err));
    cJSON_AddBoolToObject(resp, "awake", display_ili9488_is_awake());
    cJSON_AddNumberToObject(resp, "display_brightness", display_ili9488_get_brightness());
    esp_err_t send_err = send_json(req, resp);
    cJSON_Delete(resp);
    cJSON_Delete(root);
    return send_err;
}

static esp_err_t wifi_ap_post(httpd_req_t *req)
{
    char body[128];
    if (!read_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_OK;
    }

    bool enabled = true;
    cJSON *item = cJSON_GetObjectItem(root, "enabled");
    if (cJSON_IsBool(item)) {
        enabled = cJSON_IsTrue(item);
    }

    esp_err_t err = wifi_portal_set_ap_enabled(enabled);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
    cJSON_AddStringToObject(resp, "err", esp_err_to_name(err));
    cJSON_AddBoolToObject(resp, "ap_enabled", s_cfg->ap_enabled);
    esp_err_t send_err = send_json(req, resp);
    cJSON_Delete(resp);
    cJSON_Delete(root);
    return send_err;
}

static esp_err_t hardware_diagnose_post(httpd_req_t *req)
{
    (void)req;
    esp_err_t err = hardware_diag_run(s_cfg);
    hardware_diag_status_t diag = hardware_diag_get_status();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
    cJSON_AddStringToObject(resp, "err", esp_err_to_name(err));
    cJSON_AddNumberToObject(resp, "last_run_ms", diag.last_run_ms);
    add_hw_item(resp, "display", diag.started, diag.display_err, display_ili9488_is_ready());
    add_hw_item(resp, "touch", diag.started, diag.touch_err, touch_ft6336_is_ready());
    add_hw_item(resp, "nfc", diag.started, diag.nfc_err, nfc_pn532_is_ready());
    add_hw_item(resp, "tf", diag.started, diag.sd_err, storage_sd_get_status().mounted);
    add_hw_item(resp, "camera", diag.started, diag.camera_err, camera_mgr_is_active());
    esp_err_t send_err = send_json(req, resp);
    cJSON_Delete(resp);
    return send_err;
}

static esp_err_t buttons_status_get(httpd_req_t *req)
{
    button_input_status_t buttons = button_input_get_status();
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "started", buttons.started);
    cJSON_AddNumberToObject(resp, "configured_count", buttons.configured_count);
    cJSON_AddBoolToObject(resp, "up_pressed", buttons.up_pressed);
    cJSON_AddBoolToObject(resp, "down_pressed", buttons.down_pressed);
    cJSON_AddBoolToObject(resp, "ok_pressed", buttons.ok_pressed);
    cJSON_AddBoolToObject(resp, "wake_pressed", buttons.wake_pressed);
    cJSON_AddStringToObject(resp, "last_event", buttons.last_event);
    cJSON_AddNumberToObject(resp, "event_count", buttons.event_count);
    esp_err_t send_err = send_json(req, resp);
    cJSON_Delete(resp);
    return send_err;
}

static void add_ui_status_to_json(cJSON *resp)
{
    device_ui_status_t ui = device_ui_get_status();
    cJSON_AddBoolToObject(resp, "started", ui.started);
    cJSON_AddBoolToObject(resp, "awake", ui.awake);
    cJSON_AddNumberToObject(resp, "page", ui.page);
    cJSON_AddStringToObject(resp, "page_name", ui.page_name);
    cJSON_AddNumberToObject(resp, "redraw_count", ui.redraw_count);
    cJSON_AddNumberToObject(resp, "handled_button_count", ui.handled_button_count);
    cJSON_AddStringToObject(resp, "last_button_event", ui.last_button_event);
    cJSON_AddNumberToObject(resp, "touch_event_count", ui.touch_event_count);
    cJSON_AddStringToObject(resp, "last_touch_event", ui.last_touch_event);
    cJSON_AddBoolToObject(resp, "touch_range_valid", ui.touch_range_valid);
    cJSON_AddNumberToObject(resp, "last_touch_raw_x", ui.last_touch_raw_x);
    cJSON_AddNumberToObject(resp, "last_touch_raw_y", ui.last_touch_raw_y);
    cJSON_AddNumberToObject(resp, "last_touch_x", ui.last_touch_x);
    cJSON_AddNumberToObject(resp, "last_touch_y", ui.last_touch_y);
    cJSON_AddNumberToObject(resp, "touch_raw_min_x", ui.touch_raw_min_x);
    cJSON_AddNumberToObject(resp, "touch_raw_max_x", ui.touch_raw_max_x);
    cJSON_AddNumberToObject(resp, "touch_raw_min_y", ui.touch_raw_min_y);
    cJSON_AddNumberToObject(resp, "touch_raw_max_y", ui.touch_raw_max_y);
    cJSON_AddNumberToObject(resp, "touch_min_x", ui.touch_min_x);
    cJSON_AddNumberToObject(resp, "touch_max_x", ui.touch_max_x);
    cJSON_AddNumberToObject(resp, "touch_min_y", ui.touch_min_y);
    cJSON_AddNumberToObject(resp, "touch_max_y", ui.touch_max_y);
    cJSON_AddNumberToObject(resp, "touch_rotation", s_cfg ? s_cfg->touch_rotation : 2);
    cJSON_AddBoolToObject(resp, "touch_swap_xy", s_cfg ? s_cfg->touch_swap_xy : false);
    cJSON_AddBoolToObject(resp, "touch_flip_x", s_cfg ? s_cfg->touch_flip_x : false);
    cJSON_AddBoolToObject(resp, "touch_flip_y", s_cfg ? s_cfg->touch_flip_y : false);
    cJSON_AddNumberToObject(resp, "touch_raw_width", s_cfg ? s_cfg->touch_raw_width : BOARD_TOUCH_RAW_WIDTH);
    cJSON_AddNumberToObject(resp, "touch_raw_height", s_cfg ? s_cfg->touch_raw_height : BOARD_TOUCH_RAW_HEIGHT);
}

static esp_err_t ui_status_get(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    add_ui_status_to_json(resp);
    esp_err_t send_err = send_json(req, resp);
    cJSON_Delete(resp);
    return send_err;
}

static bool ui_page_from_string(const char *name, device_ui_page_t *page)
{
    if (!name || !page) {
        return false;
    }
    if (strcasecmp(name, "home") == 0) {
        *page = DEVICE_UI_PAGE_HOME;
        return true;
    }
    if (strcasecmp(name, "shortcuts") == 0 || strcasecmp(name, "quick") == 0) {
        *page = DEVICE_UI_PAGE_SHORTCUTS;
        return true;
    }
    if (strcasecmp(name, "detail") == 0 || strcasecmp(name, "part") == 0 ||
        strcasecmp(name, "partdb") == 0 || strcasecmp(name, "part-db") == 0) {
        *page = DEVICE_UI_PAGE_PARTDB;
        return true;
    }
    if (strcasecmp(name, "nfc") == 0) {
        *page = DEVICE_UI_PAGE_NFC;
        return true;
    }
    if (strcasecmp(name, "camera") == 0 || strcasecmp(name, "cam") == 0 ||
        strcasecmp(name, "qr") == 0 || strcasecmp(name, "scan") == 0) {
        *page = DEVICE_UI_PAGE_CAMERA;
        return true;
    }
    if (strcasecmp(name, "info") == 0 ||
        strcasecmp(name, "hardware") == 0 || strcasecmp(name, "hw") == 0) {
        *page = DEVICE_UI_PAGE_HARDWARE;
        return true;
    }
    if (strcasecmp(name, "settings") == 0 || strcasecmp(name, "setting") == 0) {
        *page = DEVICE_UI_PAGE_SETTINGS;
        return true;
    }
    return false;
}

static esp_err_t ui_page_post(httpd_req_t *req)
{
    char body[160];
    if (!read_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_OK;
    }

    esp_err_t err = ESP_ERR_INVALID_ARG;
    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (cJSON_IsString(action)) {
        if (strcasecmp(action->valuestring, "next") == 0) {
            err = device_ui_next_page();
        } else if (strcasecmp(action->valuestring, "prev") == 0 ||
                   strcasecmp(action->valuestring, "previous") == 0) {
            err = device_ui_prev_page();
        } else if (strcasecmp(action->valuestring, "redraw") == 0) {
            err = device_ui_request_redraw();
        } else if (strcasecmp(action->valuestring, "home") == 0) {
            err = device_ui_set_page(DEVICE_UI_PAGE_HOME);
        }
    }
    cJSON *page_item = cJSON_GetObjectItem(root, "page");
    if (cJSON_IsNumber(page_item)) {
        err = device_ui_set_page((device_ui_page_t)page_item->valueint);
    } else if (cJSON_IsString(page_item)) {
        device_ui_page_t page = DEVICE_UI_PAGE_HOME;
        if (ui_page_from_string(page_item->valuestring, &page)) {
            err = device_ui_set_page(page);
        }
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
    cJSON_AddStringToObject(resp, "err", esp_err_to_name(err));
    add_ui_status_to_json(resp);
    esp_err_t send_err = send_json(req, resp);
    cJSON_Delete(resp);
    cJSON_Delete(root);
    return send_err;
}

static esp_err_t partdb_get_post(httpd_req_t *req)
{
    char body[256];
    if (!read_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_OK;
    }

    const cJSON *path_item = cJSON_GetObjectItem(root, "path");
    const char *path = cJSON_IsString(path_item) ? path_item->valuestring : NULL;
    esp_err_t err = ESP_OK;
    int http_status = 0;
    char *response = NULL;
    if (!path || path[0] != '/' || strstr(path, "://")) {
        err = ESP_ERR_INVALID_ARG;
    } else {
        response = malloc(PARTDB_RESPONSE_MAX);
        if (!response) {
            err = ESP_ERR_NO_MEM;
        } else {
            err = partdb_client_get_json(path, response, PARTDB_RESPONSE_MAX, &http_status);
        }
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK && http_status >= 200 && http_status < 300);
    cJSON_AddStringToObject(resp, "err", esp_err_to_name(err));
    cJSON_AddStringToObject(resp, "path", path ? path : "");
    cJSON_AddNumberToObject(resp, "http_status", http_status);
    cJSON_AddStringToObject(resp, "cache_source", partdb_client_last_source());
    cJSON_AddBoolToObject(resp, "cache_hit", partdb_client_get_status().last_cache_hit);
    if (response) {
        cJSON_AddStringToObject(resp, "body", response);
    }
    ESP_LOGI(TAG, "Part-DB GET path=%s err=%s http=%d ok=%d body_len=%u",
             path ? path : "", esp_err_to_name(err), http_status,
             err == ESP_OK && http_status >= 200 && http_status < 300,
             response ? (unsigned)strlen(response) : 0);
    esp_err_t send_err = send_json(req, resp);
    free(response);
    cJSON_Delete(resp);
    cJSON_Delete(root);
    return send_err;
}

static esp_err_t nfc_partdb_read_post(httpd_req_t *req)
{
    char body[256] = {0};
    cJSON *req_json = NULL;
    if (req->content_len > 0) {
        if (!read_body(req, body, sizeof(body))) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
            return ESP_OK;
        }
        req_json = cJSON_Parse(body);
        if (!req_json) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
            return ESP_OK;
        }
    } else {
        req_json = cJSON_CreateObject();
        if (!req_json) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json alloc failed");
            return ESP_OK;
        }
    }

    char barcode[NFC_BARCODE_MAX] = {0};
    bool request_barcode = json_string_copy(req_json, "barcode", barcode, sizeof(barcode));
    esp_err_t read_err = ESP_OK;
    if (!request_barcode) {
        nfc_manual_request_begin();
        read_err = nfc_pn532_read_ndef_text(barcode, sizeof(barcode), 2500);
        nfc_manual_request_end();
    }

    cJSON *root = cJSON_CreateObject();
    partdb_barcode_target_t target = {0};
    bool parsed_internal = barcode[0] && target_from_barcode(barcode, &target);
    bool parsed_user_barcode = false;
    bool parsed_ipn = false;
    if (!parsed_internal && barcode[0]) {
        parsed_user_barcode = target_from_user_barcode_lookup(root, barcode, &target);
    }
    if (!parsed_internal && !parsed_user_barcode && barcode[0]) {
        parsed_ipn = target_from_ipn_lookup(root, barcode, &target);
    }
    bool parsed = parsed_internal || parsed_user_barcode || parsed_ipn;
    esp_err_t report_err = parsed ? read_err :
                           ((!request_barcode && read_err != ESP_OK) ? read_err : ESP_ERR_INVALID_ARG);
    cJSON_AddBoolToObject(root, "ok", read_err == ESP_OK && parsed);
    cJSON_AddStringToObject(root, "err", esp_err_to_name(report_err));
    cJSON_AddStringToObject(root, "workflow", "partdb_barcode");
    cJSON_AddStringToObject(root, "object", "partdb_barcode");
    cJSON_AddBoolToObject(root, "read_supported", true);
    cJSON_AddStringToObject(root, "source", request_barcode ? "request" : "nfc_ndef_text");
    cJSON_AddStringToObject(root, "barcode", barcode);
    cJSON_AddStringToObject(root, "barcode_kind", parsed_internal ? "internal" :
                            (parsed_user_barcode ? "part_lot_user_barcode" :
                             (parsed_ipn ? "part_ipn" : "unknown")));
    if (parsed) {
        add_target_json(root, &target);
        add_partdb_lookup_json(root, &target);
    } else {
        cJSON_AddStringToObject(root, "expected", "Part-DB internal barcode like P0001, L0001 or S0001");
    }
    esp_err_t send_err = send_json(req, root);
    cJSON_Delete(root);
    cJSON_Delete(req_json);
    return send_err;
}

static esp_err_t nfc_partdb_write_post(httpd_req_t *req)
{
    char body[256];
    if (!read_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    cJSON *req_json = cJSON_Parse(body);
    if (!req_json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_OK;
    }

    bool commit = json_bool_value(req_json, "commit", false);
    bool verify = json_bool_value(req_json, "verify", true);
    bool ensure_barcode = json_bool_value(req_json, "ensure_barcode", true);
    bool force_barcode = json_bool_value(req_json, "force_barcode", false);
    char requested_ipn[NFC_BARCODE_MAX] = {0};
    bool has_ipn = json_string_copy(req_json, "ipn", requested_ipn, sizeof(requested_ipn));
    char payload_kind[24] = {0};
    if (!json_string_copy(req_json, "payload_kind", payload_kind, sizeof(payload_kind))) {
        snprintf(payload_kind, sizeof(payload_kind), "%s", has_ipn ? "ipn" : "internal");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "workflow", "partdb_barcode");
    cJSON_AddStringToObject(root, "object", "partdb_barcode");
    cJSON_AddBoolToObject(root, "write_supported", true);
    cJSON_AddBoolToObject(root, "write_attempted", commit);
    cJSON_AddBoolToObject(root, "verified_required", verify);
    cJSON_AddBoolToObject(root, "ensure_barcode", ensure_barcode);
    cJSON_AddBoolToObject(root, "force_barcode", force_barcode);
    cJSON_AddStringToObject(root, "payload_kind", payload_kind);

    partdb_barcode_target_t target = {0};
    esp_err_t err = ESP_OK;
    if (has_ipn) {
        bool found = target_from_ipn_lookup(root, requested_ipn, &target);
        err = found ? ESP_OK : ESP_ERR_NOT_FOUND;
    } else {
        err = target_from_json(req_json, &target);
    }
    if (err == ESP_OK) {
        add_target_json(root, &target);
    } else {
        cJSON_AddStringToObject(root, "expected_input",
                                "part_id, partdb_path, Part-DB internal barcode, or ipn");
    }

    bool verified = false;
    if (err == ESP_OK && verify) {
        verified = add_partdb_lookup_json(root, &target);
        if (!verified) {
            err = ESP_ERR_NOT_FOUND;
        }
    } else if (err == ESP_OK) {
        cJSON_AddBoolToObject(root, "partdb_lookup_skipped", true);
        verified = true;
    }

    if (err == ESP_OK && ensure_barcode) {
        err = ensure_partdb_barcode_json(root, &target, commit, force_barcode);
    }

    char payload[NFC_BARCODE_MAX] = {0};
    if (err == ESP_OK) {
        if (strcasecmp(payload_kind, "ipn") == 0) {
            if (has_ipn) {
                snprintf(payload, sizeof(payload), "%s", requested_ipn);
            } else {
                err = partdb_ipn_for_target_json(root, &target, payload, sizeof(payload));
            }
        } else if (strcasecmp(payload_kind, "internal") == 0 ||
                   strcasecmp(payload_kind, "internal_barcode") == 0 ||
                   strcasecmp(payload_kind, "barcode") == 0) {
            snprintf(payload, sizeof(payload), "%s", target.barcode);
        } else {
            err = ESP_ERR_INVALID_ARG;
        }
    }

    if (err == ESP_OK && commit) {
        nfc_manual_request_begin();
        err = nfc_pn532_write_ndef_text(payload, 3000);
        nfc_manual_request_end();
    }
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK && (!commit || verified));
    cJSON_AddStringToObject(root, "err", esp_err_to_name(err));
    cJSON_AddStringToObject(root, "payload_format", "ndef_text");
    cJSON_AddStringToObject(root, "payload", err == ESP_OK || payload[0] ? payload : "");
    esp_err_t send_err = send_json(req, root);
    cJSON_Delete(root);
    cJSON_Delete(req_json);
    return send_err;
}

static esp_err_t sd_mount_get(httpd_req_t *req)
{
    esp_err_t err = storage_sd_init();
    if (err == ESP_OK) {
        err = storage_sd_prepare_paths();
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON_AddStringToObject(root, "err", esp_err_to_name(err));
    cJSON_AddBoolToObject(root, "prepared", err == ESP_OK);
    add_sd_status_to_json(root);
    storage_sd_status_t sd = storage_sd_get_status();
    ESP_LOGI(TAG, "TF mount err=%s mounted=%d total=%llu free=%llu",
             esp_err_to_name(err), sd.mounted,
             (unsigned long long)sd.total_bytes,
             (unsigned long long)sd.free_bytes);
    esp_err_t send_err = send_json(req, root);
    cJSON_Delete(root);
    return send_err;
}

static esp_err_t sd_prepare_post(httpd_req_t *req)
{
    esp_err_t err = storage_sd_prepare_paths();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON_AddStringToObject(root, "err", esp_err_to_name(err));
    cJSON_AddBoolToObject(root, "prepared", err == ESP_OK);
    add_sd_status_to_json(root);
    ESP_LOGI(TAG, "TF prepare err=%s", esp_err_to_name(err));
    esp_err_t send_err = send_json(req, root);
    cJSON_Delete(root);
    return send_err;
}

static esp_err_t fonts_list_get(httpd_req_t *req)
{
    esp_err_t err = storage_sd_prepare_paths();
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
    cJSON_AddStringToObject(resp, "err", esp_err_to_name(err));
    cJSON_AddStringToObject(resp, "dir", BOARD_SD_FONT_DIR);
    add_sd_status_to_json(resp);
    cJSON *entries = cJSON_AddArrayToObject(resp, "entries");
    int count = 0;

    if (err == ESP_OK) {
        DIR *dir = opendir(BOARD_SD_FONT_DIR);
        if (!dir) {
            err = errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
            cJSON_ReplaceItemInObject(resp, "ok", cJSON_CreateBool(false));
            cJSON_ReplaceItemInObject(resp, "err", cJSON_CreateString(esp_err_to_name(err)));
        } else {
            struct dirent *ent = NULL;
            while ((ent = readdir(dir)) != NULL && count < 80) {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0 ||
                    !font_type_allowed(ent->d_name)) {
                    continue;
                }
                char path[HTTP_SD_PATH_MAX];
                int written = snprintf(path, sizeof(path), "%s/%s", BOARD_SD_FONT_DIR, ent->d_name);
                if (written < 0 || written >= (int)sizeof(path)) {
                    continue;
                }
                struct stat st;
                if (stat(path, &st) != 0 || S_ISDIR(st.st_mode) || st.st_size <= 0 ||
                    st.st_size > FONT_UPLOAD_MAX_BYTES) {
                    continue;
                }
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "name", ent->d_name);
                cJSON_AddStringToObject(item, "path", path);
                cJSON_AddStringToObject(item, "type", "font");
                cJSON_AddNumberToObject(item, "size", (double)st.st_size);
                cJSON_AddNumberToObject(item, "mtime", (double)st.st_mtime);
                cJSON_AddItemToArray(entries, item);
                count++;
            }
            closedir(dir);
        }
    }

    cJSON_AddNumberToObject(resp, "count", count);
    esp_err_t send_err = send_json(req, resp);
    cJSON_Delete(resp);
    return send_err;
}

static esp_err_t fonts_upload_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > FONT_UPLOAD_MAX_BYTES) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "font too large");
        return ESP_OK;
    }

    char original[ASSET_NAME_MAX] = {0};
    (void)get_query_value(req, "name", original, sizeof(original));
    if (!font_type_allowed(original)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "font must be ttf/otf/ttc/woff/woff2");
        return ESP_OK;
    }

    char safe[ASSET_NAME_MAX] = {0};
    sanitize_upload_name(original, "font.ttf", safe, sizeof(safe));
    if (!font_type_allowed(safe)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad font name");
        return ESP_OK;
    }

    esp_err_t err = storage_sd_prepare_paths();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_OK;
    }

    char path[HTTP_SD_PATH_MAX];
    int written = snprintf(path, sizeof(path), "%s/%s", BOARD_SD_FONT_DIR, safe);
    if (written < 0 || written >= (int)sizeof(path)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "path too long");
        return ESP_OK;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
        return ESP_OK;
    }

    char *buf = malloc(4096);
    if (!buf) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }

    int remaining = req->content_len;
    int written_total = 0;
    while (remaining > 0) {
        int chunk = remaining > 4096 ? 4096 : remaining;
        int r = httpd_req_recv(req, buf, chunk);
        if (r <= 0) {
            err = ESP_FAIL;
            break;
        }
        if (fwrite(buf, 1, r, f) != (size_t)r) {
            err = ESP_FAIL;
            break;
        }
        remaining -= r;
        written_total += r;
    }
    free(buf);
    fclose(f);
    if (err != ESP_OK) {
        (void)remove(path);
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
    cJSON_AddStringToObject(resp, "err", esp_err_to_name(err));
    cJSON_AddStringToObject(resp, "name", safe);
    cJSON_AddStringToObject(resp, "path", path);
    cJSON_AddStringToObject(resp, "type", "font");
    cJSON_AddNumberToObject(resp, "size", written_total);
    add_sd_status_to_json(resp);
    ESP_LOGI(TAG, "font upload path=%s size=%d err=%s", path, written_total, esp_err_to_name(err));
    esp_err_t send_err = send_json(req, resp);
    cJSON_Delete(resp);
    return send_err;
}

static esp_err_t fonts_delete_post(httpd_req_t *req)
{
    char body[512];
    if (!read_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_OK;
    }

    char path[HTTP_SD_PATH_MAX] = {0};
    const cJSON *path_item = cJSON_GetObjectItem(root, "path");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    bool selected_cleared = false;
    if (cJSON_IsString(path_item) && font_path_allowed(path_item->valuestring)) {
        app_config_copy_string(path, sizeof(path), path_item->valuestring);
        err = storage_sd_prepare_paths();
        if (err == ESP_OK) {
            struct stat st;
            if (stat(path, &st) != 0 || S_ISDIR(st.st_mode) || st.st_size <= 0 ||
                st.st_size > FONT_UPLOAD_MAX_BYTES) {
                err = ESP_ERR_NOT_FOUND;
            } else if (remove(path) != 0) {
                err = ESP_FAIL;
            }
        }
    }

    if (err == ESP_OK && s_cfg && strcmp(s_cfg->font_path, path) == 0) {
        s_cfg->font_path[0] = '\0';
        esp_err_t save_err = app_config_save(s_cfg);
        selected_cleared = save_err == ESP_OK;
        if (save_err == ESP_OK) {
            ui_font_set_active_path(s_cfg->font_path);
            (void)device_ui_request_redraw();
        }
        if (save_err != ESP_OK) {
            err = save_err;
        }
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
    cJSON_AddStringToObject(resp, "err", esp_err_to_name(err));
    cJSON_AddStringToObject(resp, "path", path);
    cJSON_AddBoolToObject(resp, "selected_cleared", selected_cleared);
    add_sd_status_to_json(resp);
    ESP_LOGW(TAG, "font delete path=%s err=%s selected_cleared=%d",
             path, esp_err_to_name(err), selected_cleared);
    esp_err_t send_err = send_json(req, resp);
    cJSON_Delete(resp);
    cJSON_Delete(root);
    return send_err;
}

static esp_err_t sd_format_post(httpd_req_t *req)
{
    char body[128];
    if (!read_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_OK;
    }

    const cJSON *confirm = cJSON_GetObjectItem(root, "confirm");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(confirm) && strcmp(confirm->valuestring, "FORMAT") == 0) {
        err = storage_sd_format_and_prepare();
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
    cJSON_AddStringToObject(resp, "err", esp_err_to_name(err));
    cJSON_AddBoolToObject(resp, "formatted", err == ESP_OK);
    add_sd_status_to_json(resp);
    ESP_LOGW(TAG, "TF format requested err=%s", esp_err_to_name(err));
    esp_err_t send_err = send_json(req, resp);
    cJSON_Delete(resp);
    cJSON_Delete(root);
    return send_err;
}

static esp_err_t sd_upload_post(httpd_req_t *req)
{
    char rel_path[HTTP_SD_PATH_MAX];
    esp_err_t err = get_query_value(req, "path", rel_path, sizeof(rel_path));
    if (err != ESP_OK || !sd_relative_path_is_safe(rel_path) || strcmp(rel_path, "/") == 0 ||
        rel_path[strlen(rel_path) - 1] == '/') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_OK;
    }

    err = storage_sd_prepare_paths();
    char abs_path[HTTP_SD_PATH_MAX];
    if (err == ESP_OK) {
        err = make_sd_path(rel_path, abs_path, sizeof(abs_path));
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_OK;
    }

    FILE *f = fopen(abs_path, "wb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
        return ESP_OK;
    }

    char *buf = malloc(2048);
    if (!buf) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }

    int remaining = req->content_len;
    int written_total = 0;
    while (remaining > 0) {
        int chunk = remaining > 2048 ? 2048 : remaining;
        int r = httpd_req_recv(req, buf, chunk);
        if (r <= 0) {
            err = ESP_FAIL;
            break;
        }
        if (fwrite(buf, 1, r, f) != (size_t)r) {
            err = ESP_FAIL;
            break;
        }
        remaining -= r;
        written_total += r;
    }
    free(buf);
    fclose(f);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
    cJSON_AddStringToObject(resp, "err", esp_err_to_name(err));
    cJSON_AddStringToObject(resp, "path", rel_path);
    cJSON_AddNumberToObject(resp, "size", written_total);
    add_sd_status_to_json(resp);
    ESP_LOGI(TAG, "TF upload path=%s size=%d err=%s",
             rel_path, written_total, esp_err_to_name(err));
    esp_err_t send_err = send_json(req, resp);
    cJSON_Delete(resp);
    return send_err;
}

static esp_err_t assets_list_get(httpd_req_t *req)
{
    char kind_name[32];
    esp_err_t err = get_query_value(req, "kind", kind_name, sizeof(kind_name));
    const asset_kind_t *kind = err == ESP_OK ? asset_kind_for(kind_name) : NULL;
    if (!kind) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad kind");
        return ESP_OK;
    }

    asset_entry_t *entries = calloc(ASSET_SCAN_MAX, sizeof(asset_entry_t));
    if (!entries) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    int oldest = -1;
    int count = scan_asset_entries(kind, entries, ASSET_SCAN_MAX, &oldest);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "kind", kind->kind);
    cJSON_AddStringToObject(resp, "label", kind->label);
    cJSON_AddStringToObject(resp, "dir", kind->rel_dir);
    cJSON_AddNumberToObject(resp, "limit", kind->limit);
    cJSON_AddNumberToObject(resp, "count", count);
    const char *selected_path = selected_asset_path_const(kind);
    cJSON_AddStringToObject(resp, "selected_path", selected_path);
    cJSON *arr = cJSON_AddArrayToObject(resp, "entries");
    int shown = count < ASSET_SCAN_MAX ? count : ASSET_SCAN_MAX;
    for (int i = 0; i < shown; i++) {
        add_asset_entry_json(arr, NULL, &entries[i], selected_path);
    }
    if (oldest >= 0) {
        add_asset_entry_json(resp, "oldest", &entries[oldest], selected_path);
    }
    esp_err_t send_err = send_json(req, resp);
    cJSON_Delete(resp);
    free(entries);
    return send_err;
}

static esp_err_t validate_existing_asset_path(const char *rel_path, const asset_kind_t **out_kind)
{
    const asset_kind_t *kind = asset_kind_for_path(rel_path);
    if (!kind || !selected_asset_path_allowed(kind, rel_path)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = storage_sd_prepare_paths();
    char abs_path[HTTP_SD_PATH_MAX];
    if (err == ESP_OK) {
        err = make_sd_path(rel_path, abs_path, sizeof(abs_path));
    }
    if (err != ESP_OK) {
        return err;
    }

    struct stat st;
    if (stat(abs_path, &st) != 0 || S_ISDIR(st.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }

    if (out_kind) {
        *out_kind = kind;
    }
    return ESP_OK;
}

static esp_err_t save_selected_asset_path(const asset_kind_t *kind, const char *rel_path, bool *changed)
{
    char *selected = selected_asset_path_mut(kind);
    if (!selected || !rel_path) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(selected, rel_path) == 0) {
        if (changed) {
            *changed = false;
        }
        return ESP_OK;
    }
    app_config_copy_string(selected, APP_CONFIG_PATH_LEN, rel_path);
    if (changed) {
        *changed = true;
    }
    return app_config_save(s_cfg);
}

static esp_err_t clear_selected_asset_if_matches(const asset_kind_t *kind, const char *rel_path, bool *cleared)
{
    char *selected = selected_asset_path_mut(kind);
    if (cleared) {
        *cleared = false;
    }
    if (!selected || !rel_path || strcmp(selected, rel_path) != 0) {
        return ESP_OK;
    }
    selected[0] = '\0';
    if (cleared) {
        *cleared = true;
    }
    return app_config_save(s_cfg);
}

static esp_err_t assets_select_post(httpd_req_t *req)
{
    char body[512];
    if (!read_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_OK;
    }

    const cJSON *path_item = cJSON_GetObjectItem(root, "path");
    const char *rel_path = cJSON_IsString(path_item) ? path_item->valuestring : NULL;
    const asset_kind_t *kind = NULL;
    esp_err_t err = validate_existing_asset_path(rel_path, &kind);
    bool changed = false;
    if (err == ESP_OK) {
        err = save_selected_asset_path(kind, rel_path, &changed);
        if (err == ESP_OK && changed) {
            (void)device_ui_request_redraw();
        }
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
    cJSON_AddStringToObject(resp, "err", esp_err_to_name(err));
    cJSON_AddStringToObject(resp, "path", rel_path ? rel_path : "");
    cJSON_AddStringToObject(resp, "kind", kind ? kind->kind : "");
    cJSON_AddBoolToObject(resp, "changed", changed);
    ESP_LOGI(TAG, "asset select path=%s kind=%s err=%s changed=%d",
             rel_path ? rel_path : "", kind ? kind->kind : "", esp_err_to_name(err), changed);
    esp_err_t send_err = send_json(req, resp);
    cJSON_Delete(resp);
    cJSON_Delete(root);
    return send_err;
}

static esp_err_t assets_delete_post(httpd_req_t *req)
{
    char body[512];
    if (!read_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_OK;
    }
    const cJSON *path_item = cJSON_GetObjectItem(root, "path");
    const char *rel_path = cJSON_IsString(path_item) ? path_item->valuestring : NULL;
    const asset_kind_t *kind = asset_kind_for_path(rel_path);

    esp_err_t err = ESP_ERR_INVALID_ARG;
    bool selected_cleared = false;
    if (kind) {
        char abs_path[HTTP_SD_PATH_MAX];
        err = make_sd_path(rel_path, abs_path, sizeof(abs_path));
        if (err == ESP_OK && remove(abs_path) != 0) {
            err = errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        }
        if (err == ESP_OK) {
            esp_err_t clear_err = clear_selected_asset_if_matches(kind, rel_path, &selected_cleared);
            if (clear_err != ESP_OK) {
                err = clear_err;
            }
        }
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
    cJSON_AddStringToObject(resp, "err", esp_err_to_name(err));
    cJSON_AddStringToObject(resp, "path", rel_path ? rel_path : "");
    cJSON_AddStringToObject(resp, "kind", kind ? kind->kind : "");
    cJSON_AddBoolToObject(resp, "selected_cleared", selected_cleared);
    ESP_LOGW(TAG, "asset delete path=%s err=%s selected_cleared=%d",
             rel_path ? rel_path : "", esp_err_to_name(err), selected_cleared);
    esp_err_t send_err = send_json(req, resp);
    cJSON_Delete(resp);
    cJSON_Delete(root);
    return send_err;
}

static esp_err_t assets_upload_post(httpd_req_t *req)
{
    char kind_name[32];
    esp_err_t err = get_query_value(req, "kind", kind_name, sizeof(kind_name));
    const asset_kind_t *kind = err == ESP_OK ? asset_kind_for(kind_name) : NULL;
    if (!kind) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad kind");
        return ESP_OK;
    }

    char replace_value[8] = {0};
    bool replace_oldest = get_query_value(req, "replace_oldest", replace_value, sizeof(replace_value)) == ESP_OK &&
                          strcmp(replace_value, "1") == 0;

    char original[ASSET_NAME_MAX] = {0};
    (void)get_query_value(req, "name", original, sizeof(original));
    if (kind->animated && !asset_type_allowed(kind, original)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "boot animation must be gif or webp");
        return ESP_OK;
    }

    asset_entry_t *entries = calloc(ASSET_SCAN_MAX, sizeof(asset_entry_t));
    if (!entries) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    int oldest = -1;
    int count = scan_asset_entries(kind, entries, ASSET_SCAN_MAX, &oldest);
    if (count >= kind->limit) {
        if (replace_oldest && oldest >= 0) {
            char old_abs[HTTP_SD_PATH_MAX];
            if (make_sd_path(entries[oldest].path, old_abs, sizeof(old_abs)) == ESP_OK) {
                (void)remove(old_abs);
                bool cleared = false;
                (void)clear_selected_asset_if_matches(kind, entries[oldest].path, &cleared);
                ESP_LOGW(TAG, "asset limit replace kind=%s deleted=%s", kind->kind, entries[oldest].path);
            }
        } else {
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "ok", false);
            cJSON_AddStringToObject(resp, "err", esp_err_to_name(ESP_ERR_INVALID_SIZE));
            cJSON_AddBoolToObject(resp, "limit_exceeded", true);
            cJSON_AddStringToObject(resp, "kind", kind->kind);
            cJSON_AddStringToObject(resp, "label", kind->label);
            cJSON_AddNumberToObject(resp, "limit", kind->limit);
            cJSON_AddNumberToObject(resp, "count", count);
            if (oldest >= 0) {
                add_asset_entry_json(resp, "oldest", &entries[oldest], selected_asset_path_const(kind));
            }
            esp_err_t send_err = send_json(req, resp);
            cJSON_Delete(resp);
            free(entries);
            return send_err;
        }
    }
    free(entries);

    char safe[ASSET_NAME_MAX] = {0};
    sanitize_asset_name(original, safe, sizeof(safe));

    const char *ext = asset_upload_ext(kind, original);
    char rel_path[HTTP_SD_PATH_MAX];
    int written = snprintf(rel_path, sizeof(rel_path), "%s/%lld_%s%s",
                           kind->rel_dir, (long long)(esp_timer_get_time() / 1000), safe, ext);
    if (written < 0 || written >= (int)sizeof(rel_path)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "path too long");
        return ESP_OK;
    }

    err = storage_sd_prepare_paths();
    char abs_path[HTTP_SD_PATH_MAX];
    if (err == ESP_OK) {
        err = make_sd_path(rel_path, abs_path, sizeof(abs_path));
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_OK;
    }

    FILE *f = fopen(abs_path, "wb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
        return ESP_OK;
    }

    char *buf = malloc(2048);
    if (!buf) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }

    int remaining = req->content_len;
    int written_total = 0;
    while (remaining > 0) {
        int chunk = remaining > 2048 ? 2048 : remaining;
        int r = httpd_req_recv(req, buf, chunk);
        if (r <= 0) {
            err = ESP_FAIL;
            break;
        }
        if (fwrite(buf, 1, r, f) != (size_t)r) {
            err = ESP_FAIL;
            break;
        }
        remaining -= r;
        written_total += r;
    }
    free(buf);
    fclose(f);
    if (err != ESP_OK) {
        (void)remove(abs_path);
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
    cJSON_AddStringToObject(resp, "err", esp_err_to_name(err));
    cJSON_AddStringToObject(resp, "kind", kind->kind);
    cJSON_AddStringToObject(resp, "path", rel_path);
    cJSON_AddStringToObject(resp, "type", "image");
    cJSON_AddNumberToObject(resp, "size", written_total);
    add_sd_status_to_json(resp);
    ESP_LOGI(TAG, "asset upload kind=%s path=%s size=%d err=%s",
             kind->kind, rel_path, written_total, esp_err_to_name(err));
    esp_err_t send_err = send_json(req, resp);
    cJSON_Delete(resp);
    return send_err;
}

static esp_err_t sd_list_post(httpd_req_t *req)
{
    char body[256];
    if (!read_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_OK;
    }

    const cJSON *path_item = cJSON_GetObjectItem(root, "path");
    const char *rel_path = cJSON_IsString(path_item) ? path_item->valuestring : "/";
    char abs_path[HTTP_SD_PATH_MAX];
    esp_err_t err = storage_sd_init();
    if (err == ESP_OK) {
        err = make_sd_path(rel_path, abs_path, sizeof(abs_path));
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
    cJSON_AddStringToObject(resp, "err", esp_err_to_name(err));
    cJSON_AddStringToObject(resp, "path", rel_path ? rel_path : "/");
    add_sd_status_to_json(resp);
    cJSON *entries = cJSON_AddArrayToObject(resp, "entries");

    if (err == ESP_OK) {
        DIR *dir = opendir(abs_path);
        if (!dir) {
            err = errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
            cJSON_ReplaceItemInObject(resp, "ok", cJSON_CreateBool(false));
            cJSON_ReplaceItemInObject(resp, "err", cJSON_CreateString(esp_err_to_name(err)));
        } else {
            struct dirent *ent = NULL;
            int count = 0;
            while ((ent = readdir(dir)) != NULL && count < 80) {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                    continue;
                }
                char child_rel[HTTP_SD_PATH_MAX];
                int written = 0;
                if (strcmp(rel_path, "/") == 0) {
                    written = snprintf(child_rel, sizeof(child_rel), "/%s", ent->d_name);
                } else {
                    written = snprintf(child_rel, sizeof(child_rel), "%s/%s", rel_path, ent->d_name);
                }
                if (written < 0 || written >= (int)sizeof(child_rel)) {
                    continue;
                }
                char child_abs[HTTP_SD_PATH_MAX];
                if (make_sd_path(child_rel, child_abs, sizeof(child_abs)) != ESP_OK) {
                    continue;
                }
                struct stat st;
                if (stat(child_abs, &st) != 0) {
                    continue;
                }
                bool is_dir = S_ISDIR(st.st_mode);
                const char *type = is_dir ? "dir" : file_type_for_name(ent->d_name);
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "name", ent->d_name);
                cJSON_AddStringToObject(item, "path", child_rel);
                cJSON_AddBoolToObject(item, "dir", is_dir);
                cJSON_AddNumberToObject(item, "size", is_dir ? 0 : (double)st.st_size);
                cJSON_AddStringToObject(item, "type", type);
                cJSON_AddBoolToObject(item, "viewable", strcmp(type, "image") == 0 || strcmp(type, "text") == 0);
                cJSON_AddItemToArray(entries, item);
                count++;
            }
            closedir(dir);
        }
    }

    esp_err_t send_err = send_json(req, resp);
    cJSON_Delete(resp);
    cJSON_Delete(root);
    return send_err;
}

static esp_err_t sd_file_get(httpd_req_t *req)
{
    char rel_path[HTTP_SD_PATH_MAX];
    esp_err_t err = get_query_value(req, "path", rel_path, sizeof(rel_path));
    if (err != ESP_OK || !sd_relative_path_is_safe(rel_path) || strcmp(rel_path, "/") == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_OK;
    }

    err = storage_sd_init();
    char abs_path[HTTP_SD_PATH_MAX];
    if (err == ESP_OK) {
        err = make_sd_path(rel_path, abs_path, sizeof(abs_path));
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_OK;
    }

    struct stat st;
    if (stat(abs_path, &st) != 0 || S_ISDIR(st.st_mode)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_OK;
    }

    FILE *f = fopen(abs_path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "open failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, content_type_for_name(rel_path));
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char *buf = malloc(1024);
    if (!buf) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    size_t n = 0;
    while ((n = fread(buf, 1, 1024, f)) > 0) {
        err = httpd_resp_send_chunk(req, buf, n);
        if (err != ESP_OK) {
            break;
        }
    }
    free(buf);
    fclose(f);
    if (err == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    return ESP_OK;
}

static void uid_to_hex(const nfc_tag_t *tag, char *out, size_t out_len)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!tag || !out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    size_t needed = (size_t)tag->uid_len * 2 + 1;
    if (out_len < needed) {
        return;
    }
    for (uint8_t i = 0; i < tag->uid_len; i++) {
        out[i * 2] = hex[tag->uid[i] >> 4];
        out[i * 2 + 1] = hex[tag->uid[i] & 0x0f];
    }
    out[tag->uid_len * 2] = '\0';
}

static void nfc_manual_request_begin(void)
{
    nfc_service_suspend_for_request();
    vTaskDelay(pdMS_TO_TICKS(180));
}

static void nfc_manual_request_end(void)
{
    nfc_service_resume_after_request();
}

static esp_err_t nfc_read_get(httpd_req_t *req)
{
    esp_err_t err = ESP_OK;
    nfc_manual_request_begin();
    if (!nfc_pn532_is_ready()) {
        err = nfc_pn532_init();
    }
    nfc_tag_t tag = {0};
    if (err == ESP_OK) {
        err = nfc_pn532_read_passive(&tag, 1500);
    }
    nfc_manual_request_end();
    char uid[sizeof(tag.uid) * 2 + 1] = {0};
    uid_to_hex(&tag, uid, sizeof(uid));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK || err == ESP_ERR_NOT_FOUND);
    cJSON_AddStringToObject(root, "err", esp_err_to_name(err));
    cJSON_AddBoolToObject(root, "ready", nfc_pn532_is_ready());
    cJSON_AddBoolToObject(root, "present", tag.uid_len > 0);
    cJSON_AddStringToObject(root, "uid", uid);
    cJSON_AddNumberToObject(root, "uid_len", tag.uid_len);
    ESP_LOGI(TAG, "nfc read err=%s ready=%d present=%d uid=%s",
             esp_err_to_name(err), nfc_pn532_is_ready(), tag.uid_len > 0, uid);
    esp_err_t send_err = send_json(req, root);
    cJSON_Delete(root);
    return send_err;
}

static esp_err_t camera_jpeg_get(httpd_req_t *req)
{
    int64_t start_us = esp_timer_get_time();
    camera_fb_t *fb = NULL;
    esp_err_t err = camera_mgr_capture_jpeg(&fb);
    if (err != ESP_OK || !fb) {
        camera_mgr_deinit();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_OK;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    int64_t send_start_us = esp_timer_get_time();
    err = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    ESP_LOGI(TAG, "camera response capture_total=%lld ms send=%lld ms len=%u err=%s",
             (long long)((send_start_us - start_us) / 1000),
             (long long)((esp_timer_get_time() - send_start_us) / 1000),
             (unsigned)fb->len,
             esp_err_to_name(err));
    camera_mgr_release_frame(fb);
    camera_mgr_deinit();
    return err;
}

static esp_err_t camera_scan_post(httpd_req_t *req)
{
    (void)req;
    qr_scanner_result_t result = {0};
    esp_err_t err = qr_scanner_scan(&result);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK || err == ESP_ERR_NOT_FOUND);
    cJSON_AddStringToObject(root, "err", esp_err_to_name(err));
    cJSON_AddBoolToObject(root, "found", result.found);
    cJSON_AddStringToObject(root, "text", result.text);
    cJSON_AddNumberToObject(root, "code_count", result.code_count);
    cJSON_AddNumberToObject(root, "width", result.width);
    cJSON_AddNumberToObject(root, "height", result.height);
    cJSON_AddNumberToObject(root, "elapsed_ms", result.elapsed_ms);
    cJSON_AddStringToObject(root, "decode_error", result.decode_error);
    ESP_LOGI(TAG, "qr scan http err=%s found=%d text=%s",
             esp_err_to_name(err), result.found, result.found ? result.text : "-");
    esp_err_t send_err = send_json(req, root);
    cJSON_Delete(root);
    return send_err;
}

static esp_err_t ota_post(httpd_req_t *req)
{
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ota partition");
        return ESP_OK;
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, req->content_len, &handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
        return ESP_OK;
    }

    char buf[1024];
    int remaining = req->content_len;
    while (remaining > 0 && err == ESP_OK) {
        int r = httpd_req_recv(req, buf, remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining);
        if (r <= 0) {
            err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(handle, buf, r);
        remaining -= r;
    }

    if (err == ESP_OK) {
        err = esp_ota_end(handle);
    } else {
        esp_ota_abort(handle);
    }
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(update_partition);
    }

    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "OTA OK, rebooting");
        esp_restart();
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }
    return ESP_OK;
}

esp_err_t http_server_start(app_config_t *cfg)
{
    if (s_server) {
        return ESP_OK;
    }
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = cfg;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192;
    config.max_uri_handlers = 40;
    config.recv_wait_timeout = 60;
    config.send_wait_timeout = 15;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "http start failed");

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/api/config", .method = HTTP_GET, .handler = config_get},
        {.uri = "/api/config", .method = HTTP_POST, .handler = config_post},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/partdb/get", .method = HTTP_POST, .handler = partdb_get_post},
        {.uri = "/api/nfc/partdb/read", .method = HTTP_POST, .handler = nfc_partdb_read_post},
        {.uri = "/api/nfc/partdb/write", .method = HTTP_POST, .handler = nfc_partdb_write_post},
        {.uri = "/api/hardware/diagnose", .method = HTTP_POST, .handler = hardware_diagnose_post},
        {.uri = "/api/buttons/status", .method = HTTP_GET, .handler = buttons_status_get},
        {.uri = "/api/ui/status", .method = HTTP_GET, .handler = ui_status_get},
        {.uri = "/api/ui/page", .method = HTTP_POST, .handler = ui_page_post},
        {.uri = "/api/wifi/ap", .method = HTTP_POST, .handler = wifi_ap_post},
        {.uri = "/api/display/test", .method = HTTP_GET, .handler = display_test_get},
        {.uri = "/api/display/brightness", .method = HTTP_POST, .handler = display_brightness_post},
        {.uri = "/api/display/power", .method = HTTP_POST, .handler = display_power_post},
        {.uri = "/api/touch", .method = HTTP_GET, .handler = touch_get},
        {.uri = "/api/nfc/read", .method = HTTP_GET, .handler = nfc_read_get},
        {.uri = "/api/sd/mount", .method = HTTP_GET, .handler = sd_mount_get},
        {.uri = "/api/sd/prepare", .method = HTTP_POST, .handler = sd_prepare_post},
        {.uri = "/api/fonts/list", .method = HTTP_GET, .handler = fonts_list_get},
        {.uri = "/api/fonts/upload", .method = HTTP_POST, .handler = fonts_upload_post},
        {.uri = "/api/fonts/delete", .method = HTTP_POST, .handler = fonts_delete_post},
        {.uri = "/api/sd/format", .method = HTTP_POST, .handler = sd_format_post},
        {.uri = "/api/sd/list", .method = HTTP_POST, .handler = sd_list_post},
        {.uri = "/api/sd/file", .method = HTTP_GET, .handler = sd_file_get},
        {.uri = "/api/sd/upload", .method = HTTP_POST, .handler = sd_upload_post},
        {.uri = "/api/assets/list", .method = HTTP_GET, .handler = assets_list_get},
        {.uri = "/api/assets/select", .method = HTTP_POST, .handler = assets_select_post},
        {.uri = "/api/assets/upload", .method = HTTP_POST, .handler = assets_upload_post},
        {.uri = "/api/assets/delete", .method = HTTP_POST, .handler = assets_delete_post},
        {.uri = "/api/camera/scan", .method = HTTP_POST, .handler = camera_scan_post},
        {.uri = "/api/camera.jpg", .method = HTTP_GET, .handler = camera_jpeg_get},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = ota_post},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &routes[i]), TAG, "route failed");
    }

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}
