"""Compare fixed-camera GPU output with a CPU shading reference and optional image baseline.

Requires Pillow. Coordinates are normalized to the output dimensions. The numerical
floor probe uses the analytic environment, so allow for cube filtering and LUT
quantization. For matching aspect ratios, compare spatial images as well.
"""
import argparse
import math
from pathlib import Path
import statistics
from PIL import Image, ImageChops, ImageStat
from bake_environment import convolve, cross, dfg, normalize

def dot(a,b):
    return sum(x*y for x,y in zip(a,b))

def output(linear):
    mapped=max(linear,0)/(1+max(linear,0))
    encoded=12.92*mapped if mapped<0.0031308 else 1.055*mapped**(1/2.4)-0.055
    return round(255*encoded)

def floor_reference(u,v,aspect):
    yaw,pitch=0.40,0.51
    eye=(11*math.sin(yaw)*math.cos(pitch),0.7+11*math.sin(pitch),11*math.cos(yaw)*math.cos(pitch))
    forward=normalize((-eye[0],0.7-eye[1],-eye[2]))
    right=normalize(cross(forward,(0,1,0)))
    up=cross(right,forward)
    fov=math.tan(math.radians(24))
    ray=normalize(tuple(forward[i]+fov*(right[i]*(2*u-1)*aspect+up[i]*(1-2*v)) for i in range(3)))
    view=tuple(-x for x in ray)
    light=normalize((-4,8,4))
    half=normalize(tuple(x+y for x,y in zip(light,view)))
    nv,nl,nh,vh=view[1],light[1],max(0,half[1]),max(0,dot(view,half))
    rough=.8
    a2=rough**4
    dist=a2/(math.pi*(1+(a2-1)*nh*nh)**2)
    vis=.5/(nl*math.sqrt(nv*nv*(1-a2)+a2)+nv*math.sqrt(nl*nl*(1-a2)+a2))
    fresnel=.04+.96*(1-vh)**5
    diffuse_env=convolve((0,1,0),None)
    spec_env=convolve((-view[0],view[1],-view[2]),rough)
    a,b=(value/255 for value in dfg(nv,rough)[:2])
    return [output((base/math.pi+dist*vis*fresnel)*3*nl+
                   diffuse_env[i]*base+spec_env[i]*(.04*a+b))
            for i,base in enumerate((.28,.31,.35))]

def validate(image, reference=None):
    width,height=image.size
    def probe(label,x,y,expected,tolerance):
        actual=image.getpixel((int(x*width),int(y*height)))[:3]
        if max(abs(a-b) for a,b in zip(actual,expected))>tolerance:
            raise ValueError(f"{label}: got {actual}, expected {expected} ±{tolerance}")
    bg=[output(x/255) for x in (16,22,32)]
    probe("HDR clear -> sRGB",.5,.02,bg,3)
    for x,y,src,a in [(.04,.05,(240,50,20),220),(.095,.05,(30,240,90),200),(.04,.11,(30,60,190),210)]:
        expected=[round((s*a+d*(255-a))/255) for s,d in zip(src,bg)]
        probe("sprite orientation and blend",x,y,expected,3)
    probe("sprite transparent corner",.095,.11,bg,3)
    probe("direct BRDF + indirect environment on receiver",.5,.85,floor_reference(.5,.85,width/height),9)
    for i,value in enumerate((0,.001,.01,.05,.18,.5,1,4)):
        probe(f"gray patch {value}",(i+.5)/32,.98,[output(value)]*3,2)
    if reference:
        if abs(width/height-reference.width/reference.height)>.01:
            raise ValueError("Spatial baseline requires matching aspect ratio")
        size=(320,200)
        def scaled(i): return i.resize(size,Image.Resampling.BOX).convert("RGB")
        diff=ImageChops.difference(scaled(image),scaled(reference))
        mean=statistics.mean(ImageStat.Stat(diff).mean)
        severe=sum(max(p)>24 for p in (diff.get_flattened_data() if hasattr(diff,'get_flattened_data') else diff.getdata()))/(size[0]*size[1])
        if mean>3 or severe>.025:
            raise ValueError(f"Spatial baseline mismatch: mean {mean:.2f}/255, >24 error on {severe:.2%} of pixels")
        print(f"Baseline mean channel error {mean:.2f}/255; >24 on {severe:.2%} of pixels")
    print(f"Pixel checks passed at {width}x{height}")

if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image",type=Path)
    parser.add_argument("--reference",type=Path)
    args=parser.parse_args()
    validate(Image.open(args.image).convert("RGB"),Image.open(args.reference) if args.reference else None)
