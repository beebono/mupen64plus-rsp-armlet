import sys,glob,re,collections,os
def ranges(m):
    out=[];i=0
    while i<4096:
        if m[i]:
            j=i
            while j<4096 and m[j]: j+=1
            out.append((i,j)); i=j
        else: i+=1
    return out
def fmt(r): return ' '.join('%03x-%03x'%x for x in r)
groups=collections.defaultdict(list)
for t in sys.argv[1:]:
    for f in glob.glob('rw/%s/*.rw'%t):
        m=re.search(r'task-([0-9a-f]+)-',f)
        txt=open(f).read()
        e=re.search(r'ended at pc ([0-9a-f]+)',txt).group(1)
        groups[(t,m.group(1),e)].append(txt)
for k in sorted(groups):
    L=groups[k]
    LIB=bytearray(4096); W=bytearray(4096); LIpc=collections.defaultdict(set); cnt=collections.Counter(); ins=[]
    ver=''
    for txt in L:
        ins.append(int(re.search(r'-- (\d+) instr',txt).group(1)))
        mv=re.search(r'VER (.*)',txt)
        if mv: ver=mv.group(1)[:28]
        for l in txt.splitlines():
            p=l.split()
            if p and p[0] in ('LI','WMAY'):
                a,b=[int(x,16) for x in p[1].split('-')]
                for i in range(a,b):
                    if p[0]=='LI': LIB[i]=1; LIpc[i].add(p[2][4:]); cnt[i]+=1
                    else: W[i]=1
    print('== %s ucode %s end-pc %s: %d tasks, instr %d..%d  %s'%(k[0],k[1],k[2],len(L),min(ins),max(ins),ver))
    li=ranges(LIB)
    print('  LI :',' '.join('%03x-%03x(pc%s,%dt)'%(a,b,','.join(sorted(set().union(*[LIpc[i] for i in range(a,b)]))),max(cnt[i] for i in range(a,b))) for a,b in li))
    print('  W  :',fmt(ranges(W)))
