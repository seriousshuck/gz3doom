#ifndef __GL_RENDERER_H
#define __GL_RENDERER_H

#include "r_defs.h"
#include "v_video.h"
#include "vectors.h"
#include "r_renderer.h"
#include "r_data/matrix.h"
#include "hwrenderer/scene/hw_portal.h"
#include "gl/dynlights/gl_shadowmap.h"
#include <functional>

#ifdef _MSC_VER
#pragma warning(disable:4244)
#endif

struct particle_t;
class FCanvasTexture;
class FFlatVertexBuffer;
class FSkyVertexBuffer;
class OpenGLFrameBuffer;
struct FDrawInfo;
class FShaderManager;
class GLPortal;
class FLightBuffer;
class FSamplerManager;
class DPSprite;
class FGLRenderBuffers;
class FLinearDepthShader;
class FDepthBlurShader;
class FSSAOShader;
class FSSAOCombineShader;
class FPresentShader;
class FPresent3DCheckerShader;
class FPresent3DColumnShader; 
class FPresent3DRowShader;
class FGL2DDrawer;
class FHardwareTexture;
class FShadowMapShader;
class FCustomPostProcessShaders;
class SWSceneDrawer;
struct FRenderViewpoint;
#define NOQUEUE nullptr	// just some token to be used as a placeholder

enum
{
	DM_MAINVIEW,
	DM_OFFSCREEN,
	DM_PORTAL,
	DM_SKYPORTAL
};

class FGLRenderer
{
public:

	OpenGLFrameBuffer *framebuffer;
	//GLPortal *mClipPortal;
	GLPortal *mCurrentPortal;
	int mMirrorCount;
	int mPlaneMirrorCount;
	FShaderManager *mShaderManager;
	FSamplerManager *mSamplerManager;
	unsigned int mFBID;
	unsigned int mVAOID;
	unsigned int PortalQueryObject;

	int mOldFBID;

	FGLRenderBuffers *mBuffers;
	FGLRenderBuffers *mScreenBuffers;
	FGLRenderBuffers *mSaveBuffers;
	FLinearDepthShader *mLinearDepthShader;
	FSSAOShader *mSSAOShader;
	FDepthBlurShader *mDepthBlurShader;
	FSSAOCombineShader *mSSAOCombineShader;
	FPresentShader *mPresentShader;
	FPresent3DCheckerShader *mPresent3dCheckerShader;
	FPresent3DColumnShader *mPresent3dColumnShader;
	FPresent3DRowShader *mPresent3dRowShader;
	FShadowMapShader *mShadowMapShader;
	FCustomPostProcessShaders *mCustomPostProcessShaders;

	FShadowMap mShadowMap;

	//FRotator mAngles;

	FFlatVertexBuffer *mVBO;
	FSkyVertexBuffer *mSkyVBO;
	FLightBuffer *mLights;
	SWSceneDrawer *swdrawer = nullptr;

	FPortalSceneState mPortalState;

	bool buffersActive = false;

	float mSceneClearColor[3];

	FGLRenderer(OpenGLFrameBuffer *fb);
	~FGLRenderer() ;

	void Initialize(int width, int height);

	void ClearBorders();

	void SetupLevel();
	void ResetSWScene();

	void RenderScreenQuad();
	void PostProcessScene(int fixedcm, const std::function<void()> &afterBloomDrawEndScene2D);
	void AmbientOccludeScene(float m5);
	void UpdateCameraExposure();
	void BloomScene(int fixedcm);
	void TonemapScene();
	void ColormapScene(int fixedcm);
	void ClearTonemapPalette();
	void LensDistortScene();
	void ApplyFXAA();
	void BlurScene(float gameinfobluramount);
	void CopyToBackbuffer(const IntRect *bounds, bool applyGamma);
	void DrawPresentTexture(const IntRect &box, bool applyGamma);
	void Flush();
	void Draw2D(F2DDrawer *data);
	void RenderTextureView(FCanvasTexture *tex, AActor *Viewpoint, double FOV);
	void WriteSavePic(player_t *player, FileWriter *file, int width, int height);
	sector_t *RenderView(player_t *player);
	void DrawBlend(BlendInfo blendinfo);
	void BeginFrame();
    
    void Set3DViewport(bool mainview);
    sector_t *RenderViewpoint (FRenderViewpoint &mainvp, AActor * camera, IntRect * bounds, float fov, float ratio, float fovratio, bool mainview, bool toscreen);


	bool StartOffscreen();
	void EndOffscreen();

	void FillSimplePoly(FTexture *texture, FVector2 *points, int npoints,
		double originx, double originy, double scalex, double scaley,
		DAngle rotation, const FColormap &colormap, PalEntry flatcolor, int lightlevel, int bottomclip);

	static float GetZNear() { return 5.f; }
	static float GetZFar() { return 65536.f; }

	private: void FGLRenderer::gl_FillScreen();
};

#include "hwrenderer/scene/hw_fakeflat.h"

struct TexFilter_s
{
	int minfilter;
	int magfilter;
	bool mipmapping;
} ;


extern FGLRenderer *GLRenderer;

#endif
