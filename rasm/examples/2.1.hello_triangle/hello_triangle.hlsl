float4 vs_main(float3 position : POSITION) : SV_Position
{
    return float4(position, 1.0);
}

float4 ps_main() : SV_Target
{
    return float4(1.0, 0.5, 0.2, 1.0);
}
