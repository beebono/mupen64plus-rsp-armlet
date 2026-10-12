import sys,glob,struct,os
a,b=sys.argv[1],sys.argv[2]
def ev(f):
    d=open(f,'rb').read()
    n=struct.unpack_from('<I',d,76)[0]
    return d[len(d)-24*n:], n
fa=sorted(glob.glob(a+'/*.snap')); fb=sorted(glob.glob(b+'/*.snap'))
names=lambda L:[os.path.basename(x) for x in L]
same=0; diff=0; nev=0
for x in fa:
    y=os.path.join(b,os.path.basename(x))
    if not os.path.exists(y): continue
    ea,n1=ev(x); eb,n2=ev(y)
    nev+=n1
    if ea==eb: same+=1
    else:
        diff+=1
        if diff<=3: print('DIFF',os.path.basename(x),n1,n2)
print('%s vs %s: %d tasks compared, %d identical event lists, %d different (%d events in A)'%(a,b,same+diff,same,diff,nev))
