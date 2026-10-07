import struct
mods={'loader':'work/SLUS_004.04','intro':'work/dat/001.bin','main':'work/dat/000.bin'}
funcs={}
for l in open('gen/functions.txt'):
    a,b,n,m=l.split()[:4]; funcs.setdefault(m,[]).append((int(a,16),int(b,16)))
for m,p in mods.items():
    d=open(p,'rb').read()
    t_addr,t_size=struct.unpack_from('<II',d,0x18)
    body=d[0x800:0x800+t_size]
    fs=sorted(funcs.get(m,[]))
    cov=bytearray(t_size//4)
    for a,b in fs:
        for x in range(a,b,4):
            i=(x-t_addr)//4
            if 0<=i<len(cov): cov[i]=1
    # gaps
    gaps=[];i=0
    while i<len(cov):
        if not cov[i]:
            j=i
            while j<len(cov) and not cov[j]: j+=1
            gaps.append((i,j)); i=j
        else: i+=1
    sus=[]
    for i,j in gaps:
        words=[struct.unpack_from('<I',body,k*4)[0] for k in range(i,j)]
        nz=[w for w in words if w]
        if not nz: continue
        prolog=sum(1 for w in words if (w>>16)==0x27bd and (w&0x8000))   # addiu sp,sp,-N
        jrra=sum(1 for w in words if w==0x03e00008)
        if prolog or jrra:
            sus.append((t_addr+i*4,(j-i)*4,prolog,jrra))
    print(m,'text %08x..%08x'%(t_addr,t_addr+t_size),'functions',len(fs),'covered %.1f%%'%(100*sum(cov)/len(cov)),'gaps',len(gaps),'code-like gaps',len(sus))
    for a,n,p_,j in sus[:40]: print('   %08x %6d bytes  prologues=%d jr_ra=%d'%(a,n,p_,j))
