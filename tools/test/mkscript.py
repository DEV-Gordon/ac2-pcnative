import sys
nav=open('/home/czarate/Documents/Ac2Decomp/ac2native/nav_nomovie.txt').read().strip().split(',')
keep=[t for t in nav if t and int(t.split(':')[0])<6900]
spec=sys.argv[1:]   # entries like  circle:7000:16000:40:6
for s in spec:
    b,a,e,step,l=s.split(':')
    keep+=['%d:%s:%s'%(f,b,l) for f in range(int(a),int(e),int(step))]
keep.sort(key=lambda t:int(t.split(':')[0]))
print(','.join(keep))
