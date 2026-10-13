// gpu-test-sources: src/game/bioshock1r/menu_model.cpp
// Production catalogue, quality mapping and pixel-percent geometry; no game or XR.
#include "gpu_test_util.h"
#include "game/bioshock1r/menu_model.h"
#include "game/bioshock1r/display_modes.h"
using namespace bvr::b1r;
int main() {
    using S=menu::Setting;using L=menu::Level;
    const int saved[]={1,2,3,4,0};
    bool order=true;
    for(int i=0;i<5;++i) order &= display::kQualityOrder[i+1]==saved[i] && display::mode_index(2,saved[i])==i+1;
    gt::check(order && display::mode_index(1,4)==0,"Display mode order preserves every saved quality value","order %d",(int)order);
    gt::check(menu::spec(S::Upscaler).level==L::Basic && menu::spec(S::DlssQuality).level==L::Basic &&
        menu::spec(S::StereoMethod).level==L::Basic,"three main rendering choices are Basic","tiers %d/%d/%d",
        (int)menu::spec(S::Upscaler).level,(int)menu::spec(S::DlssQuality).level,(int)menu::spec(S::StereoMethod).level);
    gt::check(menu::spec(S::DlssModel).level==L::Advanced && menu::spec(S::TemporalTaa).level==L::Debug &&
        menu::spec(S::AfwHandsMask).level==L::Debug,"model is Advanced; TAA and AFW levers are Debug","tiers %d/%d/%d",
        (int)menu::spec(S::DlssModel).level,(int)menu::spec(S::TemporalTaa).level,(int)menu::spec(S::AfwHandsMask).level);
    uint32_t w=0,h=0;display::pixel_size(display::kReferenceW,display::kReferenceH,100,&w,&h);
    gt::check(w==2750 && h==2850,"100 percent is exactly the tester-specified 2750x2850","%ux%u",w,h);
    display::pixel_size(2400,2280,25,&w,&h);
    gt::check(w==1200 && h==1140,"25 percent pixels halves both axes (not a quarter of each)","%ux%u",w,h);
    display::pixel_size(1800,1920,400,&w,&h);
    gt::check(w==3600 && h==3840,"the percent calculation scales both axes together","%ux%u",w,h);
    display::pixel_size(2400,2280,-1,&w,&h);
    gt::check(!w && !h,"invalid percent never produces a resolution","%ux%u",w,h);
    return gt::finish("display-model");
}
