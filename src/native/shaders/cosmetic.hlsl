// Head cosmetics (the Tylenol bottle): a dense host-built mesh lit here with a
// glossy plastic model plus a rim glow, and its fading contrail ribbon. Drawn
// inside the scene pass (MSAA, scene depth) with the scene's smoothed
// view_proj rows, like spline.hlsl. Colors are authored in gamma space like
// the game's own shaders; HDR=1 encodes through the tone chain's inverse so
// the host tonemap restores them (see spline.hlsl SplineOut).
cbuffer C : register(b0) {
  float4 vp0;   // scene view_proj rows (row-vector: clip = x*vp0+y*vp1+z*vp2+w*vp3)
  float4 vp1;
  float4 vp2;
  float4 vp3;
  float4 w0;    // object -> world rows: world = (dot(w0,p), dot(w1,p), dot(w2,p))
  float4 w1;
  float4 w2;
  float4 cam;   // xyz = camera world position, w = seconds (animation)
  float4 key;   // xyz = key light direction (world, toward the light), w = gain
  float4 glow;  // rgb = rim glow color, w = rim strength
};
Texture2D<float4> tex : register(t0);
SamplerState smp : register(s0);

float3 CosmeticOut(float3 c) {
#ifdef HDR
  float3 tm = c * c * (2.0 / (1.41 * 1.41));
  float3 lo = 1.0 - sqrt(saturate(1.0 - tm));
  float3 hi = 4.0 * tm - 3.0;
  return lerp(lo, hi, step(1.0, tm));
#else
  return c;
#endif
}

struct BottleOut {
  float4 pos : SV_Position;
  float3 wp : TEXCOORD0;
  float3 n : TEXCOORD1;
  float2 uv : TEXCOORD2;
  float mat : TEXCOORD3;
};

BottleOut vs_bottle(float3 p : POSITION, float3 n : NORMAL, float2 uv : TEXCOORD0,
                    float mat : TEXCOORD1) {
  BottleOut o;
  float4 p1 = float4(p, 1.0);
  float3 wp = float3(dot(w0, p1), dot(w1, p1), dot(w2, p1));
  o.pos = wp.x * vp0 + wp.y * vp1 + wp.z * vp2 + vp3;
  o.wp = wp;
  o.n = float3(dot(w0.xyz, n), dot(w1.xyz, n), dot(w2.xyz, n));
  o.uv = uv;
  o.mat = mat;
  return o;
}

// mat: 0 bottle plastic, 1 label, 2 cap, 3 cap top (red), 4 cap arrow ring
float4 ps_bottle(BottleOut i) : SV_Target {
  float3 n = normalize(i.n);  // authored outward
  float3 v = normalize(cam.xyz - i.wp);
  float3 l = normalize(key.xyz);
  int mat = (int)(i.mat + 0.5);

  float3 albedo = float3(0.955, 0.958, 0.945);
  float gloss = 180.0, spec_k = 0.85, rough_k = 0.10;
  if (mat == 1) {
    float4 t = tex.Sample(smp, i.uv);
    albedo = t.rgb;
    gloss = 90.0; spec_k = 0.55; rough_k = 0.14;  // matte-coated paper label
  } else if (mat == 2) {
    albedo = float3(0.975, 0.975, 0.965);
    gloss = 60.0; spec_k = 0.45; rough_k = 0.18;
  } else if (mat == 3) {
    albedo = float3(0.84, 0.10, 0.125);
    gloss = 140.0; spec_k = 0.8; rough_k = 0.10;
  } else if (mat == 4) {
    albedo = float3(1.0, 1.0, 1.0);
  }
  float3 lin = pow(max(albedo, 1e-4), 2.2);

  float ndl = saturate(dot(n, l));
  float ndv = saturate(dot(n, v));
  float3 h = normalize(l + v);
  float ndh = saturate(dot(n, h));
  // Soft wrap diffuse + sky/ground hemisphere so the bottle reads as solid
  // from every side at night and noon alike.
  float wrap = saturate((dot(n, l) + 0.35) / 1.35);
  float hemi = lerp(0.28, 0.55, n.y * 0.5 + 0.5);
  float3 diffuse = lin * (wrap * 0.95 * key.w + hemi);
  // Two-lobe plastic specular: a tight clear-coat hot spot + a broad sheen.
  float fres = 0.04 + 0.96 * pow(1.0 - ndv, 5.0);
  float spec = pow(ndh, gloss) * spec_k + pow(ndh, 16.0) * rough_k;
  float3 r = reflect(-v, n);
  float3 sky = lerp(float3(0.10, 0.10, 0.12), float3(0.95, 0.97, 1.0), saturate(r.y * 0.6 + 0.5));
  float3 lit = diffuse + (spec * ndl * key.w) + sky * fres * 0.55;

  // "Other realm" rim: a slow-breathing red-pink glow hugging the silhouette.
  float rim = pow(1.0 - ndv, 2.6);
  float breathe = 0.85 + 0.15 * sin(cam.w * 2.1);
  lit += glow.rgb * (rim * glow.w * breathe);

  float3 outc = pow(saturate(lit), 1.0 / 2.2);
  return float4(CosmeticOut(outc), 1.0);
}

struct TrailOut {
  float4 pos : SV_Position;
  float4 col : COLOR0;
};

TrailOut vs_trail(float3 p : POSITION, float4 c : COLOR0) {
  TrailOut o;
  o.pos = p.x * vp0 + p.y * vp1 + p.z * vp2 + vp3;
  o.col = c;
  return o;
}

float4 ps_trail(TrailOut i) : SV_Target {
  return float4(CosmeticOut(i.col.rgb), saturate(i.col.a));
}
