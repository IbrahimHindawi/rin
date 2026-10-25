typedef struct vec3 { float x, y, z; } vec3;
typedef struct particle_desc { unsigned count; vec3 gravity; } particle_desc;
particle_desc desc = { 1000, { 0.0f, -9.8f, 0.0f } };
