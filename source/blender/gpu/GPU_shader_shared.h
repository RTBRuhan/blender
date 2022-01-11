
#ifndef USE_GPU_SHADER_CREATE_INFO
#  include "intern/gpu_shader_shared_utils.h"
#endif

struct NodeLinkData {
  float4 colors[3];
  float2 bezierPts[4];
  bool1 doArrow;
  bool1 doMuted;
  float dim_factor;
  float thickness;
  float dash_factor;
  float dash_alpha;
  float expandSize;
  float arrowSize;
};

struct NodeLinkInstanceData {
  float4 colors[6];
  float expandSize;
  float arrowSize;
  float2 pad;
};
