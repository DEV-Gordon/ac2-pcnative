import sys,glob
from PIL import Image
pref,first,out=sys.argv[1],int(sys.argv[2]),sys.argv[3]
fs=[f for f in sorted(glob.glob(pref+'*.ppm')) if int(f[len(pref):len(pref)+5])>=first]
ims=[Image.open(f).convert('RGB').resize((320,240)) for f in fs]
W=4; H=(len(ims)+W-1)//W
sheet=Image.new('RGB',(320*W,240*max(H,1)))
for i,im in enumerate(ims): sheet.paste(im,((i%W)*320,(i//W)*240))
sheet.save(out); print(len(ims), fs[0] if fs else None, fs[-1] if fs else None)
