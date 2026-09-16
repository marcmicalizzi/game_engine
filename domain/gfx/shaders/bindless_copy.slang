// Bindless test (ADR-0023): sample a texture chosen by index through a sampler chosen by index
// and write the result into a storage image chosen by index. Every resource comes from the
// global set at set 0; the indices arrive in push constants.

struct CopyParams {
  uint texture_index;
  uint sampler_index;
  uint output_index;
  uint width;
  uint height;
};

[[vk::binding(0, 0)]] Texture2D<float4> g_textures[];
[[vk::binding(1, 0)]] RWTexture2D<float4> g_storage_images[];
[[vk::binding(2, 0)]] SamplerState g_samplers[];
[[vk::push_constant]] ConstantBuffer<CopyParams> g_params;

[shader("compute")]
[numthreads(8, 8, 1)]
void copy(uint3 thread_id : SV_DispatchThreadID) {
  if (thread_id.x >= g_params.width || thread_id.y >= g_params.height) return;
  const float2 uv = (float2(thread_id.xy) + 0.5) / float2(g_params.width, g_params.height);
  const float4 value = g_textures[NonUniformResourceIndex(g_params.texture_index)].SampleLevel(
      g_samplers[NonUniformResourceIndex(g_params.sampler_index)], uv, 0);
  g_storage_images[NonUniformResourceIndex(g_params.output_index)][thread_id.xy] = value;
}
