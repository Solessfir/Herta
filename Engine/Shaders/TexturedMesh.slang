#include "MeshShading.slangh"

FMaterialSample EvaluateMaterial(FVertexOutput Input)
{
    return DefaultMaterial(Input);
}
