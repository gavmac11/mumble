import asyncio,ssl,struct,json,hashlib,time,uuid
from pathlib import Path

def varint(v):
 b=bytearray()
 while v>=128:b.append((v&127)|128);v>>=7
 b.append(v);return bytes(b)
def integer(k,v):return varint(k<<3)+varint(v)
def blob(k,v):return varint((k<<3)|2)+varint(len(v))+v
def fields(data):
 out={};i=0
 def read():
  nonlocal i
  v=0;s=0
  while True:
   b=data[i];i+=1;v|=(b&127)<<s
   if not b&128:return v
   s+=7
 while i<len(data):
  tag=read();k=tag>>3;wire=tag&7
  if wire==0:v=read()
  elif wire==2:n=read();v=data[i:i+n];i+=n
  else:raise ValueError('unsupported wire')
  out.setdefault(k,[]).append(v)
 return out
async def send(w,t,p):w.write(struct.pack('>HI',t,len(p))+p);await w.drain()
async def frame(r):
 h=await r.readexactly(6);t,n=struct.unpack('>HI',h);assert n<2*1024*1024;return t,await r.readexactly(n)
async def main():
 lab=Path('/tmp/mumble-stability-lab-20261010');context=ssl.create_default_context(cafile=str(lab/'native-mac-server/server.crt'));context.check_hostname=False
 expected=hashlib.sha256(ssl.PEM_cert_to_DER_cert((lab/'native-mac-server/server.crt').read_text())).hexdigest();clients=[]
 try:
  for role in ['sender','selected','nonrecipient']:
   r,w=await asyncio.open_connection('127.0.0.1',50566,ssl=context,server_hostname='localhost')
   assert hashlib.sha256(w.get_extra_info('ssl_object').getpeercert(binary_form=True)).hexdigest()==expected
   clients.append((role,r,w,None))
   await send(w,0,integer(1,0x010700)+blob(2,b'private recipient routing negative fixture'))
   await send(w,2,blob(1,('private-'+role+'-'+uuid.uuid4().hex[:8]).encode())+integer(5,1))
   while True:
    t,d=await asyncio.wait_for(frame(r),3)
    if t==4:raise RuntimeError('private fixture login rejected')
    if t==5:session=fields(d)[1][0];break
   clients[-1]=(role,r,w,session)
   await send(w,9,integer(25,1))
  # A ping round-trip orders every capability update before dispatch.
  for role,r,w,session in clients:
   await send(w,3,integer(1,1234))
   while True:
    t,d=await asyncio.wait_for(frame(r),3)
    if t==3:break
  transfer=bytes.fromhex('726f7574696e672d6e65676174697665');data=b'owned synthetic ciphertext fixture'+b'x'*16
  sender=clients[0];selected=clients[1][3];spoof=clients[2][3]
  await send(sender[2],28,integer(1,spoof)+blob(2,transfer)+integer(3,0)+integer(4,1)+blob(5,data)+integer(6,selected))
  observed={}
  for role,r,w,session in clients[1:]:
   received=[];deadline=time.monotonic()+1
   while time.monotonic()<deadline:
    try:t,d=await asyncio.wait_for(frame(r),deadline-time.monotonic())
    except asyncio.TimeoutError:break
    if t==28:
     f=fields(d)
     if f.get(2)==[transfer]:received.append({'actor_matches_authenticated_sender':f.get(1)==[sender[3]],'payload_matches':f.get(5)==[data]})
   observed[role]=received
  report={'server_address':'loopback-only private fixture','server_certificate_sha256':expected,'payload':'synthetic opaque relay chunk; not a valid encrypted file/session test','server_binary_runtime':'Existing isolated Mac server; source head not verified by this probe','declared_recipient_count':1,'observed':observed,'expected':'selected receives exactly one chunk; nonrecipient receives zero','negative_reproduced':len(observed['selected'])==1 and len(observed['nonrecipient'])==1}
  assert report['negative_reproduced'],report
  (lab/'recipient-routing-negative.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
 finally:
  for _,_,w,_ in clients:w.close()
  for _,_,w,_ in clients:
   try:await w.wait_closed()
   except Exception:pass
asyncio.run(asyncio.wait_for(main(),15))
