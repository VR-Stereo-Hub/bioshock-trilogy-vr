// The body origin under alternating eyes. The same stationary head and tracked
// wrists must describe one arm pose; only its render camera may change eyes.
#include "game/bioshock1r/frame_context.h"
#include <cmath>
#include <cstdio>
using namespace bvr::b1r;
static int checks=0,failed=0;
static float dist(FVector a,FVector b) {
    float x=a.x-b.x,y=a.y-b.y,z=a.z-b.z;return sqrtf(x*x+y*y+z*z);
}
static void check(bool ok,const char* what,float got,float want) {
    ++checks;if(!ok){++failed;printf("FAIL %s: %.7f expected %.7f\n",what,got,want);}
}
static FVector head(const FrameContext& c) {return {c.camX,c.camY,c.camZ};}
int main() {
    float maxBodyError=0,maxWristDelta=0,minBadShoulderShift=1000;
    int poses=0;
    const FVector origins[]={{0,0,0},{-31000,2100,8260},{64000,-65000,15000}};
    for(const auto& origin:origins)
    for(int pitch=-12000;pitch<=12000;pitch+=6000)
    for(int yaw=0;yaw<65536;yaw+=8192)
    for(int roll=-16384;roll<=16384;roll+=8192) {
        FRotator rot{pitch,yaw,roll};
        float f[3],r[3],u[3];ue_rot_basis(rot,f,r,u);
        FrameContext contexts[2];
        FVector rawEye[2];
        for(int e=0;e<2;++e) {
            int sign=e?1:-1;
            FVector eyed{origin.x+r[0]*3.2f*sign,origin.y+r[1]*3.2f*sign,origin.z+r[2]*3.2f*sign};
            FVector delta{eyed.x-origin.x,eyed.y-origin.y,eyed.z-origin.z};
            // A later common camera translation must remain in the body origin.
            rawEye[e]={eyed.x+4,eyed.y-7,eyed.z+2};
            FrameContext& c=contexts[e];
            set_frame_head_position(c,rawEye[e],delta);
            c.baseX=origin.x;c.baseY=origin.y;c.baseZ=origin.z;
            c.camPitch=pitch;c.camYaw=yaw;c.camRoll=roll;
            c.driveYawOffsetRad=static_cast<float>(yaw)/kRotUnitsPerRadian;
            c.worldScale=100;
        }
        float err=dist(head(contexts[0]),head(contexts[1]));
        if(err>maxBodyError)maxBodyError=err;
        check(err<.001f,"one head/shoulder origin for both eyes",err,0);
        FVector want{origin.x+4,origin.y-7,origin.z+2};
        check(dist(head(contexts[0]),want)<.001f,"body retains non-eye camera translation",dist(head(contexts[0]),want),0);
        const float qp[4]={0,0,0,1},lp[3]={-.2f,-.2f,-.5f},rp[3]={.2f,-.2f,-.5f};
        const float* positions[2]={lp,rp};
        for(const auto* pos:positions) {
            auto a=xr_pose_to_game(contexts[0],pos,qp),b=xr_pose_to_game(contexts[1],pos,qp);
            float wd=dist(a.loc,b.loc);if(wd>maxWristDelta)maxWristDelta=wd;
            check(wd<.001f,"controller wrist stays fixed between eyes",wd,0);
        }
        // Negative control: the old publish used the eyed camera as the head.
        // The same solver's shoulder therefore moved an IPD despite stable wrists.
        float bad=dist(rawEye[0],rawEye[1]);
        if(bad<minBadShoulderShift)minBadShoulderShift=bad;
        check(bad>6.38f,"old eye-centred body is detected",bad,6.4f);
        FrameContext mono;
        set_frame_head_position(mono,origin,FVector{});
        check(dist(head(mono),origin)==0,"mono/SR context has no shift removed",dist(head(mono),origin),0);
        ++poses;
    }
    printf("frame-context: %d checks over %d poses, %d failed (body %.6f uu, wrists %.6f, old shoulders >= %.3f)\n",
        checks,poses,failed,maxBodyError,maxWristDelta,minBadShoulderShift);
    return failed?1:0;
}
