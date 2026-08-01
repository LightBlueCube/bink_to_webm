Texture2D YTexture : register(t0);
Texture2D CrTexture : register(t1);
Texture2D CbTexture : register(t2);
Texture2D AlphaTexture : register(t3);
SamplerState YSampler : register(s0);
SamplerState CrSampler : register(s1);
SamplerState CbSampler : register(s2);
SamplerState AlphaSampler : register(s3);

struct PSInput
{
	float3 TexCoord : TEXCOORD0;
};

float4 PSMain(PSInput input) : SV_TARGET
{
	float y = YTexture.Sample(YSampler, input.TexCoord.xy).r;
	float cr = CrTexture.Sample(CrSampler, input.TexCoord.xy).r;
	float cb = CbTexture.Sample(CbSampler, input.TexCoord.xy).r;

	float3 rgb;
	rgb.r = y + cr * 1.40199995 - 0.703749001;
	rgb.g = y - cr * 0.714139998 - cb * 0.344139993 + 0.531215072;
	rgb.b = y + cb * 1.77199996 - 0.889474511;
	rgb = exp2(log2(abs(rgb)) * 2.20000005);

	float alpha = input.TexCoord.z * AlphaTexture.Sample(AlphaSampler, input.TexCoord.xy).r;
	clip(alpha - 0.001);
	return float4(rgb, alpha);
}
