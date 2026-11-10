#pragma once
// Foveated DLSS (game-independent; candidate for sharedtools\foveated_dlss).
// Hooks the NGX DLSS-SR evaluate under Streamline / the game (_nvngx.dll exports) and restricts the game's own DLSS
// feature to a centre rectangle of each eye with NGX's sub-rectangle parameters (DLSS.Render.Subrect.*,
// DLSS.Input.*.Subrect.Base.*, DLSS.Output.Subrect.Base.*). Own implementation of the technique (public NGX API).
namespace fovdlss {

struct Config {
    bool enabled = false;
    float width = 0.6f;    // centre region, fraction of the eye image (0.1 .. 1)
    float height = 0.6f;
    bool debugBorder = false;
    bool periphDlss = true;
    unsigned periphPreset = 5;   // NGX render preset for the periphery feature (1 = A ... 5 = E, 10 = J, 11 = K, 13 = M)
    unsigned centrePreset = 0;
    bool periphTaa = true;    // periphery (raw input colour) stabilised by our own TAA
    float taaBlend = 0.1f;    // weight of the new frame   // 0 = the game's preset   // periphery through a cheap preset-E DLAA pass (false: plain bilinear of the input colour)
};

void Install();                         // background: waits for _nvngx.dll, hooks D3D12 Create / Evaluate / Release
void SetConfig(const Config& c);
void SetEyeCenter(int eye, float u, float v);   // fovea centre in output uv (0..1) per eye (projection centre of the lens)
void SetThreadEye(int eye);             // host: eye of the DLSS evaluate about to run on this thread (-1 unknown)
void Report();                          // 10 s stats into the host log
bool LastRenderSize(unsigned& w, unsigned& h);   // render (input) size of the last DLSS-SR evaluate

}
