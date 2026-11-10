#include "hle/clock.h"
#include "hle/toypad_web.h"

#include "gpu/command_processor.h"
#include "gpu/gcn/translator.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include "hle/button_icons.h"
#include "hle/hle.h"
#include "hle/port_settings.h"
#include "hle/toypad.h"
#include "hle/videoout.h"
#include "gpu/vk/renderer.h"
#include "gpu/vk/device.h"
#include "ui/overlay.h"
#include "host/memory.h"

namespace hle {
bool PressPadButton(const std::string& name, uint32_t frames, int player);
bool SetDebugPad(int player, bool connected);
uint32_t PadSlotsMask();
uint64_t VideoOutFlipCount();
}

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
void CloseSocket(Socket s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
constexpr Socket kNoSocket = -1;
void CloseSocket(Socket s) { close(s); }
#endif

namespace toypad {
namespace {

// Figure IDs as stored on the tags (characters). The page adds the vehicles and gadgets, and an icon for
// each figure, from <data>/toypad/icons (made from the game's own files by port/tools/toypad_icons.py).
struct Named { uint32_t id; const char* name; };
constexpr Named kFigures[] = {
    {1, "Batman"}, {2, "Gandalf"}, {3, "Wyldstyle"}, {4, "Aquaman"}, {5, "Bad Cop"}, {6, "Bane"},
    {7, "Bart Simpson"}, {8, "Benny"}, {9, "Chell"}, {10, "Cole"}, {11, "Cragger"}, {12, "Cyborg"},
    {13, "Cyberman"}, {14, "Doc Brown"}, {15, "The Doctor"}, {16, "Emmet"}, {17, "Eris"}, {18, "Gimli"},
    {19, "Gollum"}, {20, "Harley Quinn"}, {21, "Homer Simpson"}, {22, "Jay"}, {23, "Joker"}, {24, "Kai"},
    {25, "ACU Trooper"}, {26, "Gamer Kid"}, {27, "Krusty the Clown"}, {28, "Laval"}, {29, "Legolas"},
    {30, "Lloyd"}, {31, "Marty McFly"}, {32, "Nya"}, {33, "Owen Grady"}, {34, "Peter Venkman"},
    {35, "Slimer"}, {36, "Scooby-Doo"}, {37, "Sensei Wu"}, {38, "Shaggy"}, {39, "Stay Puft"},
    {40, "Superman"}, {41, "Unikitty"}, {42, "Wicked Witch"}, {43, "Wonder Woman"}, {44, "Zane"},
    {45, "Green Arrow"}, {46, "Supergirl"}, {47, "Abby Yates"}, {48, "Finn the Human"}, {49, "Ethan Hunt"},
    {50, "Lumpy Space Princess"}, {51, "Jake the Dog"}, {52, "Harry Potter"}, {53, "Lord Voldemort"},
    {54, "Michael Knight"}, {55, "B.A. Baracus"}, {56, "Newt Scamander"}, {57, "Sonic the Hedgehog"},
    {59, "Gizmo"}, {60, "Stripe"}, {61, "E.T."}, {62, "Tina Goldstein"}, {63, "Marceline the Vampire Queen"},
    {64, "Batgirl"}, {65, "Robin"}, {66, "Sloth"}, {67, "Hermione Granger"}, {68, "Chase McCain"},
    {69, "Excalibur Batman"}, {70, "Raven"}, {71, "Beast Boy"}, {72, "Betelgeuse"}, {74, "Blossom"},
    {75, "Bubbles"}, {76, "Buttercup"}, {77, "Starfire"},
};

// The page: the Toy Pad drawn the way the game shows it (a flat grey board, the round centre pad at the top,
// L-shaped left and right zones), each lit as the game sets the pad's light, above the figure list.
// Split in two literals (MSVC's 16 KB literal limit).
const char kPage[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Toy Pad</title>
<style>
:root{--bg:#0e1020;--panel:#1a1e38;--edge:#2f3666;--text:#eef0ff;--dim:#9aa3d6;--accent:#4fc3ff;--warn:#ff6b8b}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:16px/1.4 system-ui,sans-serif}
header{padding:14px 16px;font-weight:700;font-size:20px;display:flex;justify-content:space-between;align-items:center}
header small{font-weight:400;color:var(--dim);font-size:13px}
.wrap{padding:0 16px}
svg.board{display:block;width:100%;max-width:720px;margin:0 auto;user-select:none;-webkit-user-select:none}
.zone{cursor:pointer;stroke:#34373e;stroke-width:4}
.zone.sel{stroke:var(--accent);stroke-width:9}.zone.over{stroke:#fff;stroke-width:9}
.lbl{font:700 20px system-ui,sans-serif;fill:#00000070;pointer-events:none}
.fig{cursor:pointer}.fig .x{fill:var(--warn)}.fig.real{cursor:default}
.fig .realtag{fill:#1b5fd1;stroke:#fff;stroke-width:2}.fig text.realtxt{font:800 15px system-ui;letter-spacing:1px}
.fig .body{transform-box:fill-box;transform-origin:50% 100%;animation:bob 1.8s ease-in-out infinite}
.fig.drop .body{animation:drop .5s cubic-bezier(.4,0,.7,1) both,bob 1.8s ease-in-out .5s infinite}
.fig .shadow{fill:#0000002e}
@keyframes drop{0%{transform:translateY(-220px);opacity:0}55%{transform:translateY(0) scale(1.1,.88);opacity:1}
 75%{transform:translateY(-16px) scale(.97,1.04)}100%{transform:none}}
@keyframes bob{0%,100%{transform:none}25%{transform:translateY(-7px) rotate(-4deg)}50%{transform:none}75%{transform:translateY(-7px) rotate(4deg)}}.fig text{font:700 22px system-ui;fill:#fff;pointer-events:none}
img.icon{object-fit:contain;flex:none}
button{font:inherit;border:0;border-radius:10px;padding:8px 12px;cursor:pointer;background:var(--accent);color:#04121c;font-weight:600}
.picker{padding:16px}
.picker input{width:100%;padding:12px;border-radius:10px;border:1px solid var(--edge);background:var(--panel);color:var(--text);font:inherit}
.list{display:grid;grid-template-columns:repeat(auto-fill,minmax(112px,1fr));gap:8px;margin-top:10px}
.list button{background:#252a52;color:var(--text);font-weight:500;display:flex;flex-direction:column;align-items:center;gap:6px;padding:10px 6px;font-size:13px;line-height:1.2;text-align:center}
.list button img{width:72px;height:72px;pointer-events:none}
.tabs{display:flex;gap:8px;margin-top:10px}.tabs button{background:transparent;color:var(--dim);border:1px solid var(--edge)}
.tabs button.on{background:var(--accent);color:#04121c;border-color:var(--accent)}
.bar{display:flex;flex-wrap:wrap;gap:8px;align-items:center;justify-content:space-between;margin-top:10px}.bar .tabs{margin:0}
.sort{display:flex;gap:6px;align-items:center;color:var(--dim);font-size:13px}
.sort select{font:inherit;font-size:14px;padding:7px 10px;border-radius:10px;border:1px solid var(--edge);background:var(--panel);color:var(--text)}
.list h3{grid-column:1/-1;margin:12px 2px 0;font-size:14px;color:var(--dim);font-weight:600;border-bottom:1px solid var(--edge);padding-bottom:4px}
.hint{color:var(--dim);font-size:13px;padding:8px 16px 4px;text-align:center}
</style></head><body>
<header>Toy Pad <small id="status">connecting...</small></header>
<div class="wrap"><svg class="board" viewBox="0 0 1000 510" xmlns="http://www.w3.org/2000/svg">
 <defs>
  <linearGradient id="face" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#eceef1"/><stop offset="1" stop-color="#bfc2c7"/></linearGradient>
  <linearGradient id="front" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#eef0f2"/><stop offset="1" stop-color="#a9adb3"/></linearGradient>
  <linearGradient id="shade" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff" stop-opacity=".45"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></linearGradient>
  <radialGradient id="dome"><stop offset="0" stop-color="#fff" stop-opacity=".5"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></radialGradient>
 </defs>
 <rect x="8" y="8" width="984" height="494" rx="18" fill="#1b1d22"/>
 <rect x="24" y="24" width="952" height="404" rx="6" fill="url(#face)"/>
 <rect x="24" y="434" width="952" height="56" rx="6" fill="url(#front)"/>
 <rect x="44" y="44" width="912" height="378" fill="none" stroke="#5d6168" stroke-width="3"/>
 <rect x="468" y="282" width="64" height="146" fill="#a3a7ad" stroke="#4a4e55" stroke-width="3"/>
 <g fill="#b3b6bb" stroke="#30333a" stroke-width="3"><rect x="62" y="56" width="62" height="40" rx="4"/><rect x="876" y="56" width="62" height="40" rx="4"/></g>
 <g fill="#d2d4d8" stroke="#30333a" stroke-width="2.5"><circle cx="79" cy="68" r="7"/><circle cx="107" cy="68" r="7"/><circle cx="79" cy="85" r="7"/><circle cx="107" cy="85" r="7"/>
  <circle cx="893" cy="68" r="7"/><circle cx="921" cy="68" r="7"/><circle cx="893" cy="85" r="7"/><circle cx="921" cy="85" r="7"/></g>
 <path class="zone" data-zone="2" d="M56 120H262V280H452V416H56Z"/>
 <path d="M56 120H262V280H452V416H56Z" fill="url(#shade)" pointer-events="none"/>
 <path class="zone" data-zone="3" d="M944 120H738V280H548V416H944Z"/>
 <path d="M944 120H738V280H548V416H944Z" fill="url(#shade)" pointer-events="none"/>
 <circle class="zone sel" data-zone="1" cx="500" cy="150" r="90"/>
 <g fill="none" stroke="#ffffff" stroke-opacity=".6" stroke-width="3" pointer-events="none"><circle cx="500" cy="150" r="72"/><circle cx="500" cy="150" r="54"/><circle cx="500" cy="150" r="36"/></g>
 <circle cx="500" cy="150" r="88" fill="url(#dome)" pointer-events="none"/>
 <text class="lbl" id="l2" x="72" y="146">LEFT</text><text class="lbl" id="l3" x="928" y="146" text-anchor="end">RIGHT</text>
 <text class="lbl" id="l1" x="500" y="270" text-anchor="middle">CENTRE</text>
 <rect x="462" y="414" width="76" height="80" fill="#e3000b" stroke="#16171b" stroke-width="3"/>
 <text x="500" y="463" text-anchor="middle" font-family="Arial Black,system-ui,sans-serif" font-weight="900" font-size="22" fill="#ffd400" stroke="#16171b" stroke-width="3" paint-order="stroke">LEGO</text>
 <g id="figs"></g>
</svg></div>
<div class="hint">Tap a zone, then a figure to place it there (or drag a figure onto the board). Tap a figure on the board to lift it off. The centre takes one figure, each side takes three.</div>
)HTML" R"HTML(<div class="picker">
 <input id="q" placeholder="Search figures, or type an ID number" autocomplete="off">
 <div class="bar">
  <div class="tabs"><button class="on" data-kind="character">Characters</button><button data-kind="vehicle">Vehicles &amp; gadgets</button></div>
  <label class="sort">Sort <select id="sort"><option value="default">Default</option><option value="franchise">Franchise</option><option value="alpha">A to Z</option></select></label>
 </div>
 <div class="list" id="list"></div>
</div>
<script>
let seen=new Set(),figures=[],zone=1,names={},icons={},kind='character',lastSlots='',lights=null,lightsAt=0,lastFrame=0;
const limits={1:1,2:3,3:3},zones={},shown={1:[0,0,0],2:[0,0,0],3:[0,0,0]},NS='http://www.w3.org/2000/svg';
// Where figures stand: [x, y, size] per figure, in board units.
const spots={1:[[500,150,150]],2:[[160,200,108],[160,340,108],[340,348,108]],3:[[840,200,108],[840,340,108],[660,348,108]]};
document.querySelectorAll('.zone').forEach(p=>{const n=+p.dataset.zone;zones[n]=p;
 p.onclick=()=>select(n);
 p.ondragover=e=>{e.preventDefault();p.classList.add('over');};
 p.ondragleave=()=>p.classList.remove('over');
 p.ondrop=e=>{e.preventDefault();p.classList.remove('over');const id=+e.dataTransfer.getData('text/plain');
  if(id>0){select(n);place(id);}};});
function select(n){zone=n;Object.values(zones).forEach(o=>o.classList.toggle('sel',+o.dataset.zone===n));}
async function api(p){const r=await fetch(p);return r.json();}
function el(tag,attrs){const e=document.createElementNS(NS,tag);for(const k in attrs)e.setAttribute(k,attrs[k]);return e;}
async function refresh(){
 try{const s=await api('/api/state');document.getElementById('status').textContent='connected';
  lights=s.lights;lightsAt=performance.now();
  const key=JSON.stringify(s.slots);if(key===lastSlots)return;lastSlots=key;
  const by={1:[],2:[],3:[]};s.slots.forEach(f=>by[f.zone].push(f));
  const g=document.getElementById('figs'),now=new Set();g.innerHTML='';
  for(const n of [1,2,3]){
   document.getElementById('l'+n).textContent=({1:'CENTRE',2:'LEFT',3:'RIGHT'})[n]+' '+by[n].length+'/'+limits[n];
   by[n].forEach((f,i)=>{const [x,y,sz]=spots[n][i]||spots[n][0],nm=names[f.id]||(f.id?'Figure '+f.id:'Toy Pad figure');
    // A figure just put down drops onto the pad; then every figure bobs gently (phases staggered by slot).
    const key=f.slot+':'+(f.physical?'real':f.id),fg=el('g',{class:'fig'+(seen.has(key)?'':' drop')+(f.physical?' real':'')});now.add(key);
    const t=el('title',{});t.textContent=nm+(f.physical?' - on your Toy Pad (lift it off there)':' - tap to remove');fg.appendChild(t);
    fg.appendChild(el('ellipse',{class:'shadow',cx:x,cy:y+sz*0.46,rx:sz*0.34,ry:sz*0.09}));
    const body=el('g',{class:'body'});body.style.animationDelay=seen.has(key)?(-f.slot*0.37)+'s':'0s,.5s';fg.appendChild(body);
    if(icons[f.id])body.appendChild(el('image',{href:'/icons/'+f.id+'.png',x:x-sz/2,y:y-sz/2,width:sz,height:sz}));
    else{body.appendChild(el('circle',{cx:x,cy:y,r:sz/2.4,fill:'#0006'}));const tx=el('text',{x:x,y:y+8,'text-anchor':'middle'});tx.textContent=f.id||'?';body.appendChild(tx);}
    if(f.physical){  // on the real pad: marked, and only lifted off there
     fg.appendChild(el('rect',{class:'realtag',x:x-sz*0.42,y:y+sz*0.3,width:sz*0.84,height:24,rx:12}));
     const rt=el('text',{x:x,y:y+sz*0.3+18,'text-anchor':'middle',class:'realtxt'});rt.textContent='REAL';fg.appendChild(rt);
     g.appendChild(fg);return;}
    fg.appendChild(el('circle',{class:'x',cx:x+sz*0.38,cy:y-sz*0.38,r:13}));
    const xx=el('text',{x:x+sz*0.38,y:y-sz*0.38+8,'text-anchor':'middle'});xx.textContent='×';fg.appendChild(xx);
    fg.onclick=async()=>{await api('/api/remove?slot='+f.slot);refresh();};g.appendChild(fg);});}
  seen=now;
 }catch(e){document.getElementById('status').textContent='game not reachable';}
}
// The same light model as the port (toypad.cpp LightColourAt), run here between polls.
function colourAt(l,t){if(l.fx===0)return l.rgb;
 const on=Math.max(l.on,1),off=l.fx===1?Math.max(l.off,1):on,cyc=on+off;
 let i=Math.floor(t/cyc)*2,rest=t%cyc;if(rest>=on){i++;rest-=on;}
 if(l.steps>=0&&i>=l.steps)return (l.steps&1)?l.to:l.from;
 const tgt=(i&1)?l.from:l.to;if(l.fx===1)return tgt;
 const src=(i&1)?l.to:l.from,len=(i&1)?off:on;return src.map((v,k)=>v+(tgt[k]-v)*rest/len);}
// A zone lit in colour c: the board's pale grey tinted toward a light version of c, as the game draws it.
const unlit=[220,223,227];
// LEDs look bright even at low values (the game pulses its prompts between about 5 and 22 of 255), so
// brightness follows a steep curve rather than the raw value.
function zoneFill(c){const m=Math.max(...c);if(m<0.5)return unlit;const a=1-Math.exp(-m/18);
 return unlit.map((u,k)=>{const hue=c[k]/m*255,lit=hue+(255-hue)*0.55;return Math.round(u+(lit-u)*a);});}
function paint(now){if(lights){const dt=now-lightsAt,k=Math.min(1,(now-lastFrame)/60);
 lights.forEach(l=>{const c=colourAt(l,l.t+dt),s=shown[l.zone];
  for(let i=0;i<3;i++)s[i]+=(c[i]-s[i])*k;  // a short glide, so flashes pulse like the game's pad
  zones[l.zone].setAttribute('fill','rgb('+zoneFill(s).join(',')+')');});}
 lastFrame=now;requestAnimationFrame(paint);}
Object.values(zones).forEach(z=>z.setAttribute('fill','rgb('+unlit.join(',')+')'));
requestAnimationFrame(paint);
function icon(id){return icons[id]?'<img class="icon" src="/icons/'+id+'.png" alt="">':'';}
function esc(s){return s.replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));}
document.querySelectorAll('.tabs button').forEach(b=>b.onclick=()=>{kind=b.dataset.kind;
 document.querySelectorAll('.tabs button').forEach(o=>o.classList.toggle('on',o===b));draw();});
async function place(id){const used=(lastSlots?JSON.parse(lastSlots):[]).filter(f=>f.zone===zone).length;
 if(used>=limits[zone]){const l=document.getElementById('l'+zone);l.style.fill='#d00';setTimeout(()=>l.style.fill='',600);return;}
 await api('/api/place?zone='+zone+'&id='+id);lastSlots='';refresh();}
function figButton(label,id){const b=document.createElement('button');b.innerHTML=label;b.onclick=()=>place(id);
 b.draggable=true;b.ondragstart=e=>{e.dataTransfer.setData('text/plain',String(id));e.dataTransfer.effectAllowed='copy';};return b;}
function draw(){const q=document.getElementById('q').value.trim().toLowerCase(),list=document.getElementById('list');list.innerHTML='';
 const n=parseInt(q,10);if(n>0)list.appendChild(figButton('Place ID '+n+(names[n]?' ('+esc(names[n])+')':''),n));
 const sort=document.getElementById('sort').value,byName=(a,b)=>a.name.localeCompare(b.name,undefined,{numeric:true});
 const shown=figures.filter(f=>(q?f.name.toLowerCase().includes(q):(f.id<1000)===(kind==='character')));
 if(sort==='alpha')shown.sort(byName);
 else if(sort==='franchise')shown.sort((a,b)=>(a.franchise||'~').localeCompare(b.franchise||'~')||a.id-b.id);
 let group=null;
 shown.forEach(f=>{if(sort==='franchise'&&f.franchise!==group){group=f.franchise;const h=document.createElement('h3');
   h.textContent=group||'Other';list.appendChild(h);}
  const b=figButton(icon(f.id)+'<span>'+esc(f.name)+'</span>',f.id);b.title='ID '+f.id+(f.franchise?' - '+f.franchise:'');list.appendChild(b);});}
document.getElementById('q').oninput=draw;
const sortBox=document.getElementById('sort');
try{sortBox.value=localStorage.getItem('toypad-sort')||'default';}catch(e){}
sortBox.onchange=()=>{try{localStorage.setItem('toypad-sort',sortBox.value);}catch(e){}draw();};
api('/api/figures').then(f=>{figures=f;f.forEach(x=>{names[x.id]=x.name;if(x.icon)icons[x.id]=1;});draw();refresh();});
setInterval(refresh,300);
</script></body></html>)HTML";

std::atomic<bool> g_running{false};
std::atomic<Socket> g_listen{kNoSocket};
std::thread g_thread;
int g_port = 0;
std::string g_url;

std::string Json(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o + "\"";
}

// Query parameter `key` from "a=1&b=2" (numbers only, which is all the API uses).
long Param(const std::string& query, const char* key) {
    const std::string k = std::string(key) + "=";
    for (size_t at = 0; at < query.size();) {
        const size_t end = std::min(query.find('&', at), query.size());
        if (query.compare(at, k.size(), k) == 0) return std::strtol(query.c_str() + at + k.size(), nullptr, 10);
        at = end + 1;
    }
    return -1;
}

void Send(Socket c, const char* type, const std::string& body, int status = 200, bool cache = false) {
    char head[256];
    std::snprintf(head, sizeof head,
                  "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: %s\r\n"
                  "Connection: close\r\n\r\n",
                  status, status == 200 ? "OK" : "Not Found", type, body.size(),
                  cache ? "max-age=86400" : "no-store");
    std::string all = head + body;
    for (size_t sent = 0; sent < all.size();) {
        const int n = send(c, all.data() + sent, int(all.size() - sent), 0);
        if (n <= 0) break;
        sent += size_t(n);
    }
}

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return in ? std::string(std::istreambuf_iterator<char>(in), {}) : std::string();
}

// The built-in names, plus every figure (vehicles too) from icons/figures.json when it has been made:
// [{"id": 1, "name": "Batman", "kind": "character", "icon": true, "franchise": "DC Comics"},...], one per line.
std::string StringField(const std::string& line, const char* key) {
    const std::string k = std::string("\"") + key + "\": \"";
    const size_t at = line.find(k);
    if (at == std::string::npos) return {};
    const size_t start = at + k.size(), end = line.find('"', start);
    return line.substr(start, end - start);
}

std::vector<FigureInfo> LoadFigures(const std::string& icon_dir) {
    std::vector<FigureInfo> out;
    const std::string listing = ReadFile(icon_dir + "/figures.json");
    std::istringstream lines(listing);
    for (std::string line; std::getline(lines, line);) {
        const size_t id_at = line.find("\"id\":");
        std::string name = StringField(line, "name");
        if (id_at == std::string::npos || name.empty()) continue;
        const uint32_t id = uint32_t(std::strtoul(line.c_str() + id_at + 5, nullptr, 10));
        // Year 2 figures need their add-on package (its world) installed; others come with the game.
        const std::string pack = StringField(line, "pack");
        if (!pack.empty() && !hle::AddcontInstalled(pack)) continue;
        for (const auto& f : kFigures)
            if (f.id == id) name = f.name;  // the hand-written names read better for characters
        out.push_back({id, name, StringField(line, "franchise"), line.find("\"icon\": true") != std::string::npos});
    }
    if (out.empty())
        for (const auto& f : kFigures) out.push_back({f.id, f.name, "", false});
    return out;
}

std::string FiguresJson(const std::string& icon_dir) {
    std::string j = "[";
    for (const FigureInfo& f : LoadFigures(icon_dir))
        j += (j.size() > 1 ? ",\n" : "") + std::string("{\"id\":") + std::to_string(f.id) + ",\"name\":" + Json(f.name) +
             ",\"icon\":" + (f.icon ? "true" : "false") + ",\"franchise\":" + Json(f.franchise) + "}";
    return j + "]";
}

// The pads' lights for the page: [{"zone":1,"rgb":[r,g,b],"fx":0 steady|1 flash|2 fade,"from":[..],"to":[..],
// "on":ms,"off":ms,"steps":n (-1 forever),"t":ms since the effect started}], which it animates itself.
std::string LightsJson(VirtualToyPad& pad) {
    auto rgb = [](const std::array<uint8_t, 3>& c) {
        return "[" + std::to_string(c[0]) + "," + std::to_string(c[1]) + "," + std::to_string(c[2]) + "]";
    };
    const auto lights = pad.Lights();
    std::string j = "[";
    for (int i = 0; i < 3; ++i) {
        const Light& l = lights[i];
        j += (i ? "," : "") + std::string("{\"zone\":") + std::to_string(i + 1) + ",\"rgb\":" + rgb(l.rgb) +
             ",\"fx\":" + std::to_string(int(l.fx)) + ",\"from\":" + rgb(l.from) + ",\"to\":" + rgb(l.to) +
             ",\"on\":" + std::to_string(l.on_ms) + ",\"off\":" + std::to_string(l.off_ms) +
             ",\"steps\":" + std::to_string(l.steps) + ",\"t\":" + std::to_string(l.elapsed_ms) + "}";
    }
    return j + "]";
}

void Handle(Socket c) {
    char buf[2048];
    const int n = recv(c, buf, sizeof buf - 1, 0);
    if (n <= 0) return;
    buf[n] = 0;
    // "GET /path?query HTTP/1.1"
    std::string line(buf, std::strcspn(buf, "\r\n"));
    const size_t sp1 = line.find(' '), sp2 = line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return;
    const std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const size_t q = target.find('?');
    const std::string path = target.substr(0, q), query = q == std::string::npos ? "" : target.substr(q + 1);
    VirtualToyPad& pad = Instance();
    if (path == "/") {
        Send(c, "text/html; charset=utf-8", kPage);
    } else if (path == "/api/figures") {
        Send(c, "application/json", FiguresJson(pad.FigureDir() + "/icons"));
    } else if (path.rfind("/icons/", 0) == 0 && path.size() > 11 && path.size() < 20 &&
               path.find_first_not_of("0123456789", 7) == path.size() - 4 && path.compare(path.size() - 4, 4, ".png") == 0) {
        const std::string png = ReadFile(pad.FigureDir() + path);
        if (png.empty()) Send(c, "text/plain", "not found", 404);
        else Send(c, "image/png", png, 200, true);
    } else if (path == "/api/state") {
        std::string j = "{\"slots\":[";
        bool first = true;
        for (const auto& s : pad.Slots()) {
            j += (first ? "" : ",") + std::string("{\"slot\":") + std::to_string(s.slot) + ",\"zone\":" +
                 std::to_string(int(s.zone)) + ",\"id\":" + std::to_string(s.id) +
                 (s.physical ? ",\"physical\":true}" : "}");
            first = false;
        }
        Send(c, "application/json", j + "],\"lights\":" + LightsJson(pad) + "}");
    } else if (path == "/api/lights") {
        Send(c, "application/json", LightsJson(pad));
    } else if (path == "/api/place") {
        const long zone = Param(query, "zone"), id = Param(query, "id");
        const int slot = (zone >= 1 && zone <= 3 && id > 0) ? pad.Place(Zone(zone), uint32_t(id)) : -1;
        std::fprintf(stderr, "[toypad-web] place figure %ld on zone %ld -> slot %d\n", id, zone, slot);
        Send(c, "application/json", "{\"slot\":" + std::to_string(slot) + "}");
    } else if (path.rfind("/debug/", 0) == 0 && std::getenv("LDPORT_DEBUG_HTTP")) {
        // Debug remote (LDPORT_DEBUG_HTTP=1 only): /debug/press?b=cross[&f=6][&p=2], /debug/pad, /debug/shot.
        // p is the player (1-4); a press for a player 2-4 with no controller plugs in a stand-in one.
        if (path == "/debug/press") {
            std::string b;
            if (const size_t at = query.find("b="); at != std::string::npos) b = query.substr(at + 2, query.find('&', at) - at - 2);
            const long f = Param(query, "f"), p = Param(query, "p");
            const bool ok = hle::PressPadButton(b, f > 0 ? uint32_t(f) : 6, p > 0 ? int(p) : 1);
            Send(c, "application/json", std::string("{\"ok\":") + (ok ? "true" : "false") +
                                          ",\"flip\":" + std::to_string(hle::VideoOutFlipCount()) + "}");
        } else if (path == "/debug/pad") {
            // /debug/pad?p=2&on=1|0: plug a stand-in controller in for player 2-4 (or out); without p, which
            // players are here (bit N = player N+1).
            const long p = Param(query, "p"), on = Param(query, "on");
            const bool ok = p < 0 || hle::SetDebugPad(int(p), on != 0);
            Send(c, "application/json", std::string("{\"ok\":") + (ok ? "true" : "false") +
                                          ",\"players\":" + std::to_string(hle::PadSlotsMask()) + "}");
        } else if (path == "/debug/icons") {
            // /debug/icons?set=0-4: as if the player had just used that kind of device (PlayStation, Xbox,
            // Xbox 360, Nintendo, keyboard), for the button icons; answers the set now wanted.
            const long set = Param(query, "set");
            if (set >= 0) icons::NoteInput(int(set));
            Send(c, "application/json", std::string("{\"wanted\":\"") + icons::SetName(icons::Wanted()) + "\",\"ready\":" +
                                          (icons::Ready() ? "true" : "false") + "}");
        } else if (path == "/debug/key") {
            // /debug/key?k=up|down|left|right|enter|escape|space|f1: an SDL key press (for the overlay).
            std::string k;
            if (const size_t at = query.find("k="); at != std::string::npos) k = query.substr(at + 2, query.find('&', at) - at - 2);
            static const std::pair<const char*, SDL_Scancode> kKeys[] = {
                {"up", SDL_SCANCODE_UP}, {"down", SDL_SCANCODE_DOWN}, {"left", SDL_SCANCODE_LEFT},
                {"right", SDL_SCANCODE_RIGHT}, {"enter", SDL_SCANCODE_RETURN}, {"escape", SDL_SCANCODE_ESCAPE},
                {"space", SDL_SCANCODE_SPACE}, {"tab", SDL_SCANCODE_TAB}, {"f11", SDL_SCANCODE_F11}};
            bool ok = false;
            for (const auto& [name, sc] : kKeys)
                if (k == name) {
                    for (bool down : {true, false}) {
                        SDL_Event e{};
                        e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                        e.key.scancode = sc;
                        e.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
                        e.key.down = down;
                        int n = 0;  // the game window's (the overlay ignores events for other windows)
                        if (SDL_Window** ws = SDL_GetWindows(&n)) {
                            if (n > 0) e.key.windowID = SDL_GetWindowID(ws[0]);
                            SDL_free(ws);
                        }
                        SDL_PushEvent(&e);
                        SDL_Delay(30);
                    }
                    ok = true;
                }
            Send(c, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
        } else if (path == "/debug/picker") {  // opens the in-game Toy Pad picker
            ui::OpenPicker();
            Send(c, "application/json", "{\"ok\":true}");
        } else if (path == "/debug/present") {
            // The window image as shown (with the overlay): run/shots/present_<w>x<h>.raw, BGRA.
            const bool ok = vkr::GetPresenter().CaptureNext(3000);
            Send(c, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
        } else if (path == "/debug/capture") {  // the next frame's draws and all its targets
            const uint64_t frame = vkr::GetRenderer().CaptureFrame(10000);
            Send(c, "application/json", "{\"frame\":" + std::to_string(frame) + "}");
        } else if (path == "/debug/freeze") {  // /debug/freeze?on=1|0
            hle::SetFrameFreeze(Param(query, "on") != 0);
            Send(c, "application/json", "{\"ok\":true}");
        } else if (path == "/debug/skip") {  // /debug/skip?from=&to= (to=0 renders everything again)
            gpu::SetDebugDrawSkip(uint32_t(Param(query, "from")), uint32_t(Param(query, "to")));
            Send(c, "application/json", "{\"ok\":true}");
        } else if (path == "/debug/set") {  // /debug/set?aa=1 | vsync=0..2 | aspect=0..2 | scale=1..16 (not saved)
            if (query.find("aa=") != std::string::npos) port::Get().aa = uint32_t(Param(query, "aa"));
            if (query.find("vsync=") != std::string::npos) port::Get().vsync = uint32_t(Param(query, "vsync"));
            if (query.find("aspect=") != std::string::npos) port::Get().aspect = uint32_t(Param(query, "aspect"));
            if (query.find("nocap=") != std::string::npos) hle::SetUncapped(Param(query, "nocap") != 0);
            if (query.find("streams=") != std::string::npos) port::Get().streams_volume = uint32_t(Param(query, "streams"));
            if (query.find("effects=") != std::string::npos) port::Get().effects_volume = uint32_t(Param(query, "effects"));
            if (query.find("f2dbg=") != std::string::npos) vkr::SetFsr2Debug(int(Param(query, "f2dbg")));
            if (query.find("scale=") != std::string::npos) {  // resolution mode 1-16
                port::Get().scale = uint32_t(Param(query, "scale"));
                if (port::FsrRatio(port::Get().scale.load()) <= 0.0f) vkr::SetRenderScale(float(port::Get().scale.load()));
            }
            Send(c, "application/json", "{\"ok\":true}");
        } else if (path == "/debug/ps") {  // /debug/ps?spec=<ps address>:<vgpr>,<vgpr>,<vgpr> (empty: normal output)
            const size_t at = query.find("spec=");
            gcn::SetDebugPsSpec(at == std::string::npos ? std::string() : query.substr(at + 5, query.find('&', at) - at - 5));
            Send(c, "application/json", "{\"ok\":true}");
        } else if (path == "/debug/mem") {  // /debug/mem?a=<hex address>&n=<bytes, max 64K>: hex dump of memory
            auto hex_param = [&](const char* key) -> uint64_t {
                const size_t at = query.find(std::string(key) + "=");
                return at == std::string::npos ? 0 : std::strtoull(query.c_str() + at + std::strlen(key) + 1, nullptr, 16);
            };
            const uint64_t a = hex_param("a");
            const size_t n = std::min<size_t>(size_t(Param(query, "n")), 65536);
            std::vector<uint8_t> buf(n);
            size_t got = 0;
            std::string out;
            if (n && host::SafeCopy(buf.data(), reinterpret_cast<const void*>(a), n)) {
                got = n;
                static const char* kHex = "0123456789abcdef";
                for (size_t i = 0; i < got; ++i) {
                    out += kHex[buf[i] >> 4];
                    out += kHex[buf[i] & 15];
                    if (i % 32 == 31) out += '\n';
                }
            }
            Send(c, "text/plain", out.empty() ? std::string("unreadable") : out);
        } else if (path == "/debug/findf") {  // /debug/findf?v=<float>: addresses of that float in private RW memory
            const float v = float(std::strtod(query.c_str() + query.find("v=") + 2, nullptr));
            std::string out;
            int found = 0;
#ifdef _WIN32
            MEMORY_BASIC_INFORMATION mbi{};
            for (uint8_t* p = nullptr; VirtualQuery(p, &mbi, sizeof mbi) && found < 400;
                 p = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize) {
                const bool rw = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                                (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY));
                if (!rw || mbi.RegionSize > (1ull << 31)) continue;
                const float* f = static_cast<const float*>(mbi.BaseAddress);
                for (size_t i = 0; i < mbi.RegionSize / 4 && found < 400; ++i)
                    if (f[i] == v) {
                        char line[40];
                        std::snprintf(line, sizeof line, "%llx\n", (unsigned long long)(uintptr_t)(f + i));
                        out += line;
                        ++found;
                    }
            }
#endif
            Send(c, "text/plain", out.empty() ? std::string("none") : out);
        } else if (path == "/debug/pokef") {  // /debug/pokef?a=<hex>&v=<float>: write a float (experiments)
            const uint64_t a = std::strtoull(query.c_str() + query.find("a=") + 2, nullptr, 16);
            const float v = float(std::strtod(query.c_str() + query.find("v=") + 2, nullptr));
            uint8_t probe[4];
            const bool ok = host::SafeCopy(probe, reinterpret_cast<const void*>(a), 4);
            if (ok) std::memcpy(reinterpret_cast<void*>(a), &v, 4);
            Send(c, "text/plain", ok ? "ok" : "failed");
        } else if (path == "/debug/shot") {
            const uint64_t frame = vkr::GetRenderer().Screenshot(3000);
            Send(c, "application/json", "{\"frame\":" + std::to_string(frame) + "}");
        } else {
            Send(c, "text/plain", "not found", 404);
        }
    } else if (path == "/api/remove") {
        const bool ok = pad.Remove(int(Param(query, "slot")));
        std::fprintf(stderr, "[toypad-web] remove slot %ld -> %s\n", Param(query, "slot"), ok ? "ok" : "refused (empty, or on the physical pad)");
        Send(c, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
    } else {
        Send(c, "text/plain", "not found", 404);
    }
}

std::string LanAddress() {
    char host[256] = {};
    if (gethostname(host, sizeof host) != 0) return "127.0.0.1";
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0) return "127.0.0.1";
    std::string best = "127.0.0.1";
    for (addrinfo* a = res; a; a = a->ai_next) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr, ip, sizeof ip);
        const std::string s = ip;
        if (s.rfind("127.", 0) == 0 || s.rfind("169.254.", 0) == 0) continue;
        best = s;
        if (s.rfind("192.168.", 0) == 0 || s.rfind("10.", 0) == 0) break;  // prefer a home network address
    }
    freeaddrinfo(res);
    return best;
}

void Serve() {
    while (g_running) {
        const Socket c = accept(g_listen, nullptr, nullptr);
        if (c == kNoSocket) break;
        Handle(c);
        CloseSocket(c);
    }
}

}  // namespace

bool StartWeb(int port) {
    if (g_running) return true;
#ifdef _WIN32
    static bool wsa = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    if (!wsa) return false;
#endif
    const Socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSocket) return false;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(uint16_t(port));
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(s, 8) != 0) {
        std::fprintf(stderr, "[toypad-web] could not listen on port %d\n", port);
        CloseSocket(s);
        return false;
    }
    g_listen = s;
    g_port = port;
    g_url = "http://" + LanAddress() + ":" + std::to_string(port);
    g_running = true;
    g_thread = std::thread(Serve);
    std::fprintf(stderr, "[toypad-web] Toy Pad page at %s\n", g_url.c_str());
    return true;
}

void StopWeb() {
    if (!g_running) return;
    g_running = false;
    CloseSocket(g_listen.exchange(kNoSocket));  // unblocks accept()
    if (g_thread.joinable()) g_thread.join();
    g_url.clear();
    std::fprintf(stderr, "[toypad-web] stopped\n");
}

bool WebRunning() { return g_running; }

std::vector<FigureInfo> FigureList() { return LoadFigures(Instance().FigureDir() + "/icons"); }
std::string WebUrl() { return g_running ? g_url : std::string(); }

}  // namespace toypad
