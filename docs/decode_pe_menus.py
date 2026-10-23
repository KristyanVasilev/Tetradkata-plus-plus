import sys; sys.stdout.reconfigure(encoding="utf-8")
import json,sys,struct
ev=json.load(open(sys.argv[1],encoding='utf-8')); exe=open(sys.argv[2],'rb').read()
res=ev['normalized_result']['resources']
def rd(r): o=r['payload']['location']; return exe[o['offset']:o['offset']+o['bytes']]
def wstr(b,i):
    j=i
    while b[j:j+2]!=b'\0\0': j+=2
    return b[i:j].decode('utf-16le'),j+2
def menu(b):
    out=[];i=4;depth=0
    while i<len(b):
        flags,=struct.unpack_from('<H',b,i);i+=2
        if flags&0x10: t,i=wstr(b,i);out.append('  '*depth+'['+t+']');depth+=1
        else:
            mid,=struct.unpack_from('<H',b,i);i+=2;t,i=wstr(b,i);out.append('  '*depth+(f'{mid}: {t}' if t else '----'))
        if flags&0x80:
            depth-=1
            # pop extra levels handled by subsequent flags
    return out
def menu_ex(b):
    out=[]
    def items(i,depth):
        while True:
            typ,state,mid,flags=struct.unpack_from('<IIIH',b,i);i+=14
            t,i=wstr(b,i); i=(i+3)&~3
            if flags&1:
                out.append('  '*depth+'['+t+']'); i+=4; i=items(i,depth+1)
            else: out.append('  '*depth+(f'{mid}: {t}' if t else '----'))
            if flags&0x80: return i
    off=struct.unpack_from('<H',b,2)[0]; items(4+off,0); return out
for r in res:
    tid=r['type'].get('id'); nm=r['name']
    if tid==4:
        b=rd(r); print('=== MENU',nm, 'lang',r['language'])
        try: lines=menu_ex(b) if struct.unpack_from('<H',b,0)[0]==1 else menu(b)
        except Exception as e: lines=['ERR '+repr(e)]
        print('\n'.join(lines))
for r in res:
    if r['type'].get('id')==5:
        b=rd(r)
        if struct.unpack_from('<HH',b,0)==(1,0xFFFF):
            i=26
            def sz(i):
                if b[i:i+2]==b'\xff\xff': return '',i+4
                if b[i:i+2]==b'\0\0': return '',i+2
                return wstr(b,i)
            _,i=sz(i);_,i=sz(i);cap,i=sz(i)
        else:
            i=18
            def sz(i):
                if b[i:i+2]==b'\xff\xff': return '',i+4
                return wstr(b,i)
            _,i=sz(i);_,i=sz(i);cap,i=sz(i)
        print('DIALOG',r['name'].get('id',r['name']),repr(cap))
