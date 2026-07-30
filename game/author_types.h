#ifndef KALAM_GAME_AUTHOR_TYPES_H
#define KALAM_GAME_AUTHOR_TYPES_H

#include <stdint.h>

#define Author_Field_Id_Scene_Cube_Color UINT64_C(0xFBBC4B32039ACAA3)
#define Author_Field_Id_Scene_Translation UINT64_C(0xD64B424ED7B113DD)
#define Author_Field_Id_Scene_Rotation_Degrees UINT64_C(0x49830D3F1E838C86)
#define Author_Field_Id_Scene_Scale UINT64_C(0x54933698B7BCB5A8)
#define Author_Field_Id_Scene_Transform UINT64_C(0x6D749360A602CC48)

#define Author_Field_Id_Post_Exposure UINT64_C(0x3D78D5D77D31FF5C)
#define Author_Field_Id_Post_Bloom_Threshold UINT64_C(0x550D9A2254BBF8D0)
#define Author_Field_Id_Post_Vignette UINT64_C(0xCA90EB5BE0B79037)
#define Author_Field_Id_Post_Tint UINT64_C(0x7F385D886CC5CB10)

//@kalam schema symbol=Scene id="nveditor.scene/v1" version=1 flags=7
typedef struct Authored_Transform
{
    //@kalam field key="scene.cube.transform.translation" name="translation" path="$.cube.transform.translation" group="cube/transform" kind=Vec3 semantic=Position min=[-10000,-10000,-10000,0] max=[10000,10000,10000,0] step=[0.01,0.01,0.01,0] flags=3 id=auto
    float translation[3];

    //@kalam field key="scene.cube.transform.rotation_degrees" name="rotation_degrees" path="$.cube.transform.rotation_degrees" group="cube/transform" kind=Euler_Degrees3 semantic=Rotation min=[-360000,-360000,-360000,0] max=[360000,360000,360000,0] step=[0.1,0.1,0.1,0] flags=3 id=auto
    float rotation_degrees[3];

    //@kalam field key="scene.cube.transform.scale" name="scale" path="$.cube.transform.scale" group="cube/transform" kind=Vec3 semantic=Scale min=[0.001,0.001,0.001,0] max=[1000,1000,1000,0] step=[0.01,0.01,0.01,0] flags=3 id=auto
    float scale[3];
} Authored_Transform;

typedef struct Authored_Scene
{
    //@kalam field key="scene.cube.color" name="color" path="$.cube.color" group="cube" kind=Color4 semantic=Color min=[0,0,0,0] max=[1,1,1,1] step=[0.01,0.01,0.01,0.01] flags=1 id=auto
    float cube_color[4];

    //@kalam field key="scene.cube.transform" name="transform" path="$.cube.transform" group="cube" kind=Transform semantic=Transform min=[0,0,0,0] max=[0,0,0,0] step=[0,0,0,0] flags=3 id=auto
    Authored_Transform cube_transform;
} Authored_Scene;
//@kalam end

//@kalam schema symbol=Post_Process id="nveditor.post_process/v1" version=1 flags=3
typedef struct Authored_Post_Process
{
    //@kalam field key="post_process.exposure" name="exposure" path="$.exposure" group="" kind=F32 semantic=None min=[0,0,0,0] max=[16,0,0,0] step=[0.01,0,0,0] flags=1 id=auto
    float exposure;

    //@kalam field key="post_process.bloom_threshold" name="bloom_threshold" path="$.bloom_threshold" group="" kind=F32 semantic=None min=[0,0,0,0] max=[16,0,0,0] step=[0.01,0,0,0] flags=1 id=auto
    float bloom_threshold;

    //@kalam field key="post_process.vignette" name="vignette" path="$.vignette" group="" kind=F32 semantic=Percentage min=[0,0,0,0] max=[1,0,0,0] step=[0.01,0,0,0] flags=1 id=auto
    float vignette;

    //@kalam field key="post_process.tint" name="tint" path="$.tint" group="" kind=Color4 semantic=Color min=[0,0,0,0] max=[1,1,1,1] step=[0.01,0.01,0.01,0.01] flags=1 id=auto
    float tint[4];
} Authored_Post_Process;
//@kalam end

#endif
