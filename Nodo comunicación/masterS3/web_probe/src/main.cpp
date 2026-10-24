// Cliente de prueba para masterS3: se graba en la SEGUNDA placa S3 (la del esclavo),
// se asocia a GeoNetwork, baja la SPA completa, abre /ws y espera el ACK de A1.
// Luego prueba el lock de un cliente: un segundo /ws recibe busy, el dueno sigue
// vivo, y /ws?takeover=1 (misma IP) lo reemplaza.
// -DWEBTEST_FRAGMENT_WS=1 manda el comando byte a byte (regresion del panic de 3.6.0).
// Despues regrabar slaveS3 en esa placa.
#ifndef WEBTEST_FRAGMENT_WS
#define WEBTEST_FRAGMENT_WS 0
#endif
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>

struct WsConn {
 WiFiClient c;
 uint8_t buf[4096];
 size_t bl = 0;
 uint8_t bin[8192];
 size_t n = 0;          // bytes binarios acumulados
 String text;           // tramas de texto concatenadas
 unsigned pings = 0, frames = 0;
 bool closeFrame = false;
 String trace;
};

static void wsSend(WiFiClient& c,uint8_t op,const uint8_t* p,size_t n){uint8_t f[140];uint8_t m[4]={0x11,0x22,0x33,0x44};f[0]=0x80|op;f[1]=0x80|n;memcpy(f+2,m,4);for(size_t i=0;i<n;i++)f[6+i]=p[i]^m[i%4];if(op==0x1 && WEBTEST_FRAGMENT_WS){for(size_t i=0;i<6+n;i++){c.write(f[i]);c.flush();delay(2);}}else c.write(f,6+n);}

// Una pasada sobre la conexion: responde ping con pong, acumula binario y texto.
static void wsPoll(WsConn& w){
 while(w.c.available()&&w.bl<sizeof(w.buf))w.buf[w.bl++]=w.c.read();
 for(;;){if(w.bl<2)break;size_t len=w.buf[1]&0x7F,hl=2;if(len==126){if(w.bl<4)break;len=(w.buf[2]<<8)|w.buf[3];hl=4;}if(w.bl<hl+len)break;
  uint8_t op=w.buf[0]&0x0F;w.frames++;if(w.trace.length()<200){w.trace+=String(w.buf[0],HEX)+":"+String((unsigned)len)+" ";}
  if(op==0x9){w.pings++;wsSend(w.c,0xA,w.buf+hl,len);}
  else if(op==0x2){size_t k=min(len,sizeof(w.bin)-w.n);memcpy(w.bin+w.n,w.buf+hl,k);w.n+=k;}
  else if(op==0x1){w.text+=String((const char*)w.buf+hl,len);w.text+='\n';}
  else if(op==0x8){w.closeFrame=true;}
  memmove(w.buf,w.buf+hl+len,w.bl-hl-len);w.bl-=hl+len;}
}

// Atiende varias conexiones a la vez durante ms (el dueno no debe quedar sin pong).
static void wsPumpAll(WsConn* const* ws,size_t count,uint32_t ms){
 uint32_t t=millis();
 while(millis()-t<ms){for(size_t i=0;i<count;i++)if(ws[i])wsPoll(*ws[i]);delay(1);}
}

static bool wsOpen(WsConn& w,const char* path){
 if(!w.c.connect("192.168.4.1",80))return false;
 String req=String("GET ")+path+" HTTP/1.1\r\nHost: 192.168.4.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
 w.c.print(req);
 String hdr;uint32_t start=millis();
 while(millis()-start<3000 && !hdr.endsWith("\r\n\r\n")){if(w.c.available())hdr+=(char)w.c.read();else delay(1);}
 return hdr.indexOf("101 Switching Protocols")>=0 && hdr.indexOf("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")>=0;
}

static bool hasAck(const WsConn& w,size_t from){
 const uint8_t expected[6]={0x56,0xff,0x07,0xa1,0,0};
 for(size_t i=from;i+6<=w.n;i+=6)if(memcmp(w.bin+i,expected,6)==0)return true;
 return false;
}

static void sendA1(WsConn& w){const char* payload="{\"cmd\":\"A1\",\"param\":0}";wsSend(w.c,0x1,(const uint8_t*)payload,strlen(payload));}


void setup() {
 Serial.begin(115200);delay(500);
 WiFi.mode(WIFI_STA);
 uint8_t ap[6]={0};
 int count=WiFi.scanNetworks(); int channel=0;
 for(int i=0;i<count;i++)if(WiFi.SSID(i)=="GeoNetwork"){Serial.printf("WEBTEST SCAN %s channel=%d rssi=%d\n",WiFi.BSSIDstr(i).c_str(),WiFi.channel(i),WiFi.RSSI(i));if(!channel){channel=WiFi.channel(i);memcpy(ap,WiFi.BSSID(i),6);}}
 WiFi.scanDelete();
 WiFi.begin("GeoNetwork","geophone2026",channel,ap);
 uint32_t start=millis();while(WiFi.status()!=WL_CONNECTED && millis()-start<15000)delay(50);
 if(WiFi.status()!=WL_CONNECTED){Serial.println("WEBTEST FAIL associate");return;}
 Serial.printf("WEBTEST AP %s\n",WiFi.BSSIDstr().c_str());
 const char* paths[]={"/health","/","/css/style.css","/js/app.js","/js/protocol.js","/js/ws_client.js","/js/data_store.js","/js/config.js","/js/plot.js","/js/enlace.js","/js/slave_panel.js","/js/spectrum.js","/js/export.js","/js/signal_proc.js","/js/zip_store.js"};
 bool ok=true;
 for(const char* p:paths){
  HTTPClient h;h.begin(String("http://192.168.4.1")+p);int code=h.GET();size_t total=0;bool html=false,lfs=false;WiFiClient* st=h.getStreamPtr();int want=h.getSize();uint32_t t0=millis();char chunk[512];while(h.connected()&&(want<0||(int)total<want)&&millis()-t0<8000){size_t av=st->available();if(!av){delay(1);continue;}int r=st->readBytes(chunk,min(av,sizeof(chunk)));String part(chunk,r);if(part.indexOf("<html")>=0)html=true;if(part.indexOf("littlefs=ok")>=0)lfs=true;total+=r;}
  bool pass=code==200 && total>0 && (want<0||(int)total==want);
  if(String(p)=="/health")pass=pass && lfs;
  if(String(p)=="/")pass=pass && html;
  Serial.printf("WEBTEST %s %s status=%d bytes=%u/%d\n",pass?"PASS":"FAIL",p,code,(unsigned)total,want);ok &=pass;h.end();
 }

 // Se crean despues del HTTP, como clientes locales.
 WsConn& a=*new WsConn; WsConn& b=*new WsConn; WsConn& t=*new WsConn;
 // 1) Dueno A: upgrade + ACK de A1.
 bool ws=wsOpen(a,"/ws");
 Serial.printf("WEBTEST %s websocket_upgrade\n",ws?"PASS":"FAIL");ok &=ws;
 WsConn* onlyA[]={&a};
 wsPumpAll(onlyA,1,1500);size_t before=a.n;
 sendA1(a);wsPumpAll(onlyA,1,4000);
 bool ack=hasAck(a,before);Serial.printf("WEBTEST TRACE A bl=%u avail=%d [%s]\n",(unsigned)a.bl,a.c.available(),a.trace.c_str());
 Serial.printf("WEBTEST %s websocket_command_ack frames=%u pings=%u bin_before=%u bin_after=%u connected=%d\n",ack?"PASS":"FAIL",a.frames,a.pings,(unsigned)before,(unsigned)(a.n-before),a.c.connected());ok &=ack;

 #ifdef WEBTEST_ONLY_STEP1
 a.c.stop();WiFi.disconnect(true);Serial.printf("WEBTEST RESULT %s\n",ok?"PASS":"FAIL");return;
 #endif
 // 2) Segundo cliente B (misma IP, dueno vivo): debe recibir busy y quedar cerrado.
 bool bUp=wsOpen(b,"/ws");
 WsConn* ab[]={&a,&b};
 wsPumpAll(ab,2,2500);
 bool busy=bUp && b.text.indexOf("busy")>=0 && (b.closeFrame||!b.c.connected());
 Serial.printf("WEBTEST %s ws_second_client_busy up=%d text=[%s] close=%d connected=%d\n",busy?"PASS":"FAIL",bUp,b.text.c_str(),b.closeFrame,b.c.connected());ok &=busy;
 b.c.stop();

 // 3) El dueno A sigue atendido tras el rechazo de B.
 before=a.n;sendA1(a);wsPumpAll(onlyA,1,3000);
 bool aAlive=a.c.connected() && !a.closeFrame && hasAck(a,before);
 Serial.printf("WEBTEST %s ws_owner_survives_busy connected=%d close=%d\n",aAlive?"PASS":"FAIL",a.c.connected(),a.closeFrame);ok &=aAlive;

 // 4) Takeover explicito: T reemplaza a A y pasa a recibir los ACK.
 bool tUp=wsOpen(t,"/ws?takeover=1");
 WsConn* at[]={&a,&t};
 wsPumpAll(at,2,2000);
 bool aGone=a.closeFrame||!a.c.connected();
 before=t.n;sendA1(t);wsPumpAll(at,2,3000);
 bool tAck=hasAck(t,before);
 bool take=tUp && aGone && tAck && t.text.indexOf("busy")<0;
 Serial.printf("WEBTEST %s ws_takeover up=%d old_closed=%d new_ack=%d text=[%s]\n",take?"PASS":"FAIL",tUp,aGone,tAck,t.text.c_str());ok &=take;

 a.c.stop();t.c.stop();WiFi.disconnect(true);Serial.printf("WEBTEST RESULT %s\n",ok?"PASS":"FAIL");
}
void loop(){delay(1000);}
