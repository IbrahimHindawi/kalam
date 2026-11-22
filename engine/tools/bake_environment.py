"""Bake Kalam's analytic sky into linear RGBA8 cubes and a split-sum BRDF LUT."""
import math
from pathlib import Path

PI = math.pi
SIZE = 32
SAMPLES = 128
OUT = Path(__file__).resolve().parents[1] / "assets/environment"


def normalize(v):
    scale = math.sqrt(sum(x*x for x in v))
    return tuple(x / scale for x in v)


def cross(a, b):
    return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])


def direction(face, x, y, size):
    u, v = 2*(x+0.5)/size - 1, 2*(y+0.5)/size - 1
    return normalize(((1,-v,-u),(-1,-v,u),(u,1,v),(u,-1,-v),(u,-v,1),(-u,-v,-1))[face])


def sky(d):
    # Analytic ground and sky with warm horizon. The sun is shaded separately.
    horizon, pole = (0.45, 0.38, 0.32), ((0.16, 0.34, 0.72) if d[1] >= 0 else (0.08, 0.07, 0.06))
    return tuple(a + (b-a)*abs(d[1]) for a,b in zip(horizon,pole))


def radical_inverse(i):
    result, weight = 0.0, 0.5
    while i:
        result += (i & 1)*weight
        i >>= 1
        weight *= 0.5
    return result


points = [((i+0.5)/SAMPLES, radical_inverse(i)) for i in range(SAMPLES)]


def sample_local(u, v, roughness):
    if roughness is None:  # cosine-weighted hemisphere
        z = math.sqrt(1-v)
    else:  # GGX importance-sampled half-vector
        z = math.sqrt((1-v)/(1+(roughness**4-1)*v))
    radius = math.sqrt(max(0,1-z*z))
    return math.cos(2*PI*u)*radius, math.sin(2*PI*u)*radius, z


def rgba(values):
    return bytes(round(max(0,min(1,x))*255) for x in (*values,1))


def convolve(n, roughness):
    if roughness == 0:
        return sky(n)
    t = normalize(cross((0,1,0) if abs(n[1]) < 0.99 else (1,0,0), n))
    b = cross(n, t)
    rgb = [0.0]*3
    weights = 0
    for u,v in points:
        x,y,z = sample_local(u,v,roughness)
        weight = 1
        if roughness is not None:  # view = normal; reflect about the half-vector
            x,y,z = 2*z*x,2*z*y,2*z*z-1
            weight = z
        if weight <= 0:
            continue
        color = sky(tuple(t[k]*x+b[k]*y+n[k]*z for k in range(3)))
        for k in range(3):
            rgb[k] += color[k]*weight
        weights += weight
    return tuple(c/weights for c in rgb)


def cube(size, mips, diffuse):
    result = bytearray()
    for face in range(6):
        for level in range(mips):
            width = size >> level
            for y in range(width):
                for x in range(width):
                    result.extend(rgba(convolve(direction(face,x,y,width), None if diffuse else level/(mips-1))))
    return result


def dfg(nv, rough):
    vx = math.sqrt(1-nv*nv)
    a = b = 0
    for u,v in points:
        hx,_,nh = sample_local(u,v,rough)
        vh = max(0,vx*hx+nv*nh)
        nl = 2*vh*nh-nv
        if nl <= 0:
            continue
        a2 = rough**4
        vis = 0.5/(nl*math.sqrt(nv*nv*(1-a2)+a2)+nv*math.sqrt(nl*nl*(1-a2)+a2))
        g = 4*nl*vis*vh/nh
        f = (1-vh)**5
        a += (1-f)*g
        b += f*g
    return rgba((a/SAMPLES,b/SAMPLES,0))


if __name__ == "__main__":
    OUT.mkdir(parents=True,exist_ok=True)
    (OUT/"irradiance.rgba8").write_bytes(cube(4,1,True))
    (OUT/"prefilter.rgba8").write_bytes(cube(16,5,False))
    (OUT/"dfg.rgba8").write_bytes(b"".join(dfg((x+.5)/SIZE,(y+.5)/SIZE) for y in range(SIZE) for x in range(SIZE)))
