#ifdef KALAM_SKINNED
#include "skinning.slang"
#endif
cbuffer Shadow { float4x4 u_modelViewProj; };
[shader("vertex")]
float4 vsMain(float3 position : POSITION
#ifdef KALAM_SKINNED
              , float4 joints : TEXCOORD1, float4 weights : TEXCOORD2
#endif
) : SV_Position {
#ifdef KALAM_SKINNED
    position = skinPosition(position, joints, weights);
#endif
    return mul(u_modelViewProj, float4(position,1));
}
[shader("fragment")]
float4 fsMain(float4 position : SV_Position) : SV_Target {
    return float4(position.zzz, 1);
}
