#ifndef KALAM_TESTS_REFLECTION_AUTHORED_FIXTURE_H
#define KALAM_TESTS_REFLECTION_AUTHORED_FIXTURE_H

//@kalam schema symbol=Scene_Fixture id="nveditor.scene/v1" version=1 flags=0
typedef struct Reflection_Fixture_Scene
{
    //@kalam field key="fixture.scene.color" name="color" path="$.cube.color" group="cube" kind=Color3 semantic=Color min=[0,0,0,0] max=[1,1,1,0] step=[0.01,0.01,0.01,0] flags=0 id=auto
    float color[3];

    //@kalam field key="fixture.scene.translation" name="translation" path="$.cube.translation" group="cube" kind=Vec3 semantic=Position min=[-10,-10,-10,0] max=[10,10,10,0] step=[0.1,0.1,0.1,0] flags=0 id=0x1001
    float translation[3];
} Reflection_Fixture_Scene;
//@kalam end

//@kalam schema symbol=Post_Process_Fixture id="nveditor.post_process/v1" version=1 flags=0
typedef struct Reflection_Fixture_Post_Process
{
    //@kalam field key="fixture.post.exposure" name="exposure" path="$.exposure" group="tone_mapping" kind=F32 semantic=None min=[0,0,0,0] max=[16,0,0,0] step=[0.05,0,0,0] flags=0 id=auto
    float exposure;
} Reflection_Fixture_Post_Process;
//@kalam end

#endif /* KALAM_TESTS_REFLECTION_AUTHORED_FIXTURE_H */
