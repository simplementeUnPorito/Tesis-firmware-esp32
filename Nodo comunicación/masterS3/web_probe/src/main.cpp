// Cliente de prueba para masterS3: se graba en la SEGUNDA placa S3 (la del esclavo),
// se asocia a GeoNetwork, baja la SPA completa, abre /ws y espera el ACK de A1.
// -DWEBTEST_FRAGMENT_WS=1 manda el comando byte a byte (regresion del panic de 3.6.0).
// Despues regrabar slaveS3 en esa placa.
#ifndef WEBTEST_FRAGMENT_WS
#define WEBTEST_FRAGMENT_WS 0
#endif
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
static void wsSend(WiFiClient& c,uint8_t op,const uint8_t* p,size_t n){uint8_t f[140];uint8_t m[4]={0x11,0x22,0x33,0x44};f[0]=0x80|op;f[1]=0x80|n;memcpy(f+2,m,4);for(size_t i=0;i<n;i++)f[6+i]=p[i]^m[i%4];if(op==0x1 && WEBTEST_FRAGMENT_WS){for(size_t i=0;i<6+n;i++){c.write(f[i]);c.flush();delay(2);}}else c.write(f,6+n);}
// Lee tramas del servidor durante ms; responde ping con pong y acumula binario.
static size_t wsPump(WiFiClient& c,uint32_t ms,uint8_t* bin,size_t cap,size_t n,unsigned& pings,unsigned& frames){
 static uint8_t buf[4096];static size_t bl=0;uint32_t t=millis();
 while(millis()-t<ms){
  while(c.available()&&bl<sizeof(buf))buf[bl++]=c.read();
  for(;;){if(bl<2)break;size_t len=buf[1]&0x7F,hl=2;if(len==126){if(bl<4)break;len=(buf[2]<<8)|buf[3];hl=4;}if(bl<hl+len)break;
   uint8_t op=buf[0]&0x0F;frames++;
   if(op==0x9){pings++;wsSend(c,0xA,buf+hl,len);}
   else if(op==0x2){size_t k=min(len,cap-n);memcpy(bin+n,buf+hl,k);n+=k;}
   memmove(buf,buf+hl+len,bl-hl-len);bl-=hl+len;}
  delay(1);}
 return n;}
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
  HTTPClient h;h.begin(String("http://192.168.4.1")+p);int code=h.GET();size_t total=0;bool html=false,lfs=false;WiFiClient* st=h.getStreamPtr();int want=h.getSize();uint32_t t0=millis();char chunk[512];while(h.connected()&&(want<0||(int)total<want)&&millis()-t0<8000){size_t a=st->available();if(!a){delay(1);continue;}int r=st->readBytes(chunk,min(a,sizeof(chunk)));String part(chunk,r);if(part.indexOf("<html")>=0)html=true;if(part.indexOf("littlefs=ok")>=0)lfs=true;total+=r;}
  bool pass=code==200 && total>0 && (want<0||(int)total==want);
  if(String(p)=="/health")pass=pass && lfs;
  if(String(p)=="/")pass=pass && html;
  Serial.printf("WEBTEST %s %s status=%d bytes=%u/%d\n",pass?"PASS":"FAIL",p,code,(unsigned)total,want);ok &=pass;h.end();
 }
 WiFiClient c;
 bool ws=c.connect("192.168.4.1",80);
 c.print("GET /ws HTTP/1.1\r\nHost: 192.168.4.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
 String hdr;start=millis();
 while(millis()-start<3000 && !hdr.endsWith("\r\n\r\n")){if(c.available())hdr+=(char)c.read();else delay(1);}
 ws &= hdr.indexOf("101 Switching Protocols")>=0 && hdr.indexOf("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")>=0;
 Serial.printf("WEBTEST %s websocket_upgrade\n",ws?"PASS":"FAIL");ok &=ws;
 static uint8_t bin[8192];size_t n=0;unsigned pings=0,frames=0;
 n=wsPump(c,1500,bin,sizeof(bin),n,pings,frames);size_t before=n;
 const char* payload="{\"cmd\":\"A1\",\"param\":0}";wsSend(c,0x1,(const uint8_t*)payload,strlen(payload));
 n=wsPump(c,4000,bin,sizeof(bin),n,pings,frames);
 bool ack=false;const uint8_t expected[6]={0x56,0xff,0x07,0xa1,0,0};
 for(size_t i=before;i+6<=n;i+=6)if(memcmp(bin+i,expected,6)==0)ack=true;
 Serial.printf("WEBTEST %s websocket_command_ack frames=%u pings=%u bin_before=%u bin_after=%u connected=%d\n",ack?"PASS":"FAIL",frames,pings,(unsigned)before,(unsigned)(n-before),c.connected());ok &=ack;
 c.stop();WiFi.disconnect(true);Serial.printf("WEBTEST RESULT %s\n",ok?"PASS":"FAIL");
}
void loop(){delay(1000);}