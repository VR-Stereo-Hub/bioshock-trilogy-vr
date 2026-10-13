#include "game/bioshock1r/afw_motion.h"
#include <cstdio>
#include <limits>
#include <initializer_list>
using namespace bvr::b1r;
static int checks=0, failures=0;
static void check(bool ok,const char* why,double got=0) {
    ++checks;if(!ok){++failures;printf("FAIL %s %.9f\n",why,got);}
}
static double distance(const float* a,const FVector& b) {
    return std::sqrt((a[0]-b.x)*(a[0]-b.x)+(a[1]-b.y)*(a[1]-b.y)+(a[2]-b.z)*(a[2]-b.z));
}
int main() {
    double worst=0,control=0;
    for(float x : {0.f,-31000.f,64000.f})
    for(int yaw=0;yaw<65536;yaw+=8192)
    for(float recenter : {0.f,.7f}) {
        FrameContext a;a.vrDriving=true;a.baseX=x;a.baseY=2100;a.baseZ=8260;
        a.camYaw=yaw;a.driveYawOffsetRad=.15f;a.recenterYawRad=recenter;
        a.recenterPx=.1f;a.recenterPy=1.6f;a.recenterPz=-.2f;
        FrameContext b=a;b.baseX+=3;b.baseY-=7;b.baseZ+=2;b.camYaw+=2500;
        const float quat[4]={0,0,0,1};
        const float points[3][3]={{-.25f,1.3f,-.45f},{.21f,1.2f,-.6f},{.03f,1.7f,-.05f}};
        for(const auto& pos:points) {
            const auto pa=xr_pose_to_game(a,pos,quat),pb=xr_pose_to_game(b,pos,quat);
            tgpu::Camera c;c.loc[0]=pa.loc.x;c.loc[1]=pa.loc.y;c.loc[2]=pa.loc.z;
            ue_rot_basis(FRotator{1700,yaw,-700},c.fwd,c.right,c.up);
            tgpu::Camera moved;
            check(afw::transport_foreground_camera(c,afw::tracking_frame(a),afw::tracking_frame(b),moved),"valid tracking transform");
            const double err=distance(moved.loc,pb.loc);worst=std::fmax(worst,err);
            check(err<.02,"matches the controller mapping after walking and turning",err);
            control=std::fmax(control,distance(c.loc,pb.loc));
            const double d=(double(b.camYaw-a.camYaw)/kRotUnitsPerRadian);
            check(std::fabs(moved.fwd[0]-(std::cos(d)*c.fwd[0]-std::sin(d)*c.fwd[1]))<1e-5,"rotates the foreground camera basis");
            FrameContext head=a;head.camYaw+=4096;head.driveYawOffsetRad+=4096/kRotUnitsPerRadian;
            head.camX+=10;head.camZ+=5;head.camPitch+=3000;
            check(afw::transport_foreground_camera(c,afw::tracking_frame(a),afw::tracking_frame(head),moved),"physical head move keeps a valid room frame");
            check(distance(moved.loc,pa.loc)<.02,"physical head motion does not drag foreground history",distance(moved.loc,pa.loc));
        }
    }
    check(control>10,"negative control detects world-fixed foreground",control);
    afw::TrackingFrame good;good.valid=true;tgpu::Camera c,out;
    afw::TrackingFrame bad=good;bad.valid=false;
    check(!afw::transport_foreground_camera(c,bad,good,out),"missing source tracking frame refused");
    check(!afw::transport_foreground_camera(c,good,bad,out),"missing current tracking frame refused");
    bad=good;bad.scale=120;
    check(!afw::transport_foreground_camera(c,good,bad,out),"changed scale cannot reuse old depth");
    bad=good;bad.yaw=std::numeric_limits<float>::quiet_NaN();
    check(!afw::transport_foreground_camera(c,good,bad,out),"nonfinite tracking frame refused");
    printf("afw-motion: %d checks, %d failed; worst mapped error %.6f uu, fixed-world control %.3f uu\n",checks,failures,worst,control);
    return failures?1:0;
}
