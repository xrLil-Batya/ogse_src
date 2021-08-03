#include "r2_rendertarget.h"
#include "r2.h"
#include "RenderDevice.h"
#include "Backend.h"
#include "xr_render_console.h"
#include "effect_rain.h"
#include "perf_misc.h"

#define STENCIL_CULL 0

extern int reset_frame;

float	hclip(float v, float dim)		{ return 2.f*v/dim - 1.f; }
bool	is_there_sun ()
{
	Fcolor					sun_color			= ((light*)RImplementation.Lights.sun_adapted._get())->color;
	return (ps_r2_ls_flags.test(R2FLAG_SUN) && (u_diffuse2s(sun_color.r,sun_color.g,sun_color.b)>EPS));
}
void	CRenderTarget::phase_combine	()
{
	BEGIN_PERF_EVENT(D3DCOLOR_XRGB(255,0,0), L"PHASE_COMBINE");
//	if (reset_frame==Device.dwFrame || reset_frame==Device.dwFrame - 1)		return;
	bool	_menu_pp	= IsMainMenuActive();//g_pGamePersistent?g_pGamePersistent->OnRenderPPUI_query():false;

	u32			Offset					= 0;
	Fvector2	p0,p1;

	//*** exposure-pipeline
	u32			gpu_id	= Device.dwFrame%2;
	{
		t_LUM_src->surface_set		(rt_LUM_pool[gpu_id*2+0]->pSurface);
		t_LUM_dest->surface_set		(rt_LUM_pool[gpu_id*2+1]->pSurface);
	}

//	if(!_menu_pp && ps_r2_ls_flags.test(R2FLAG_WET_SURFACES) && R2RM_NORMAL==ps_Render_mode)	phase_reflections_preprocess();
	if(!_menu_pp && ps_r2_pp_flags.test(R2PP_FLAG_SSAO) && R2RM_NORMAL==ps_Render_mode)			phase_ssao();

	//rain map
//	g_pGamePersistent->Environment().eff_Rain->phase_rmap();

	// low/hi RTs
	u_setrt				( rt_Generic_0,rt_Generic_1,0,HW.pBaseZB );
	RCache.set_CullMode	( CULL_NONE );
	RCache.set_Stencil	( FALSE		);

/*	BOOL	split_the_scene_to_minimize_wait			= FALSE;
	if (ps_r2_ls_flags->test(R2FLAG_EXP_SPLIT_SCENE))	split_the_scene_to_minimize_wait=TRUE;*/

	// draw skybox
	if (1)
	{
		RCache.set_ColorWriteEnable					();
		CHK_DX(HW.pDevice->SetRenderState			( D3DRS_ZENABLE,	FALSE				));
		g_pGamePersistent->Environment().RenderSky	();
		CHK_DX(HW.pDevice->SetRenderState			( D3DRS_ZENABLE,	TRUE				));
	}
	render_simple_quad(rt_rain, s_combine->E[3], 1);
	u_setrt				( rt_Generic_0,rt_Generic_1,0,HW.pBaseZB );

	//if (RImplementation.o.bug)	{
		RCache.set_Stencil					(TRUE,D3DCMP_LESSEQUAL,0x01,0xff,0x00);	// stencil should be >= 1
		if (RImplementation.o.nvstencil)	{
			u_stencil_optimize				(FALSE);
			RCache.set_ColorWriteEnable		();
		}
	//}

	// Draw full-screen quad textured with our scene image
	if (g_pGamePersistent && !g_pGamePersistent->OnRenderPPUI_query())		// тут так сделано для того, чтобы была нормальная картинка при создании сейва
	{
		// Compute params
		Fmatrix		m_v2w;			m_v2w.invert				(Device.mView		);
		CEnvDescriptorMixer& envdesc= g_pGamePersistent->Environment().CurrentEnv		;
		const float minamb			= 0.001f;
		Fvector4	ambclr			= { _max(envdesc.ambient.x*2,minamb),	_max(envdesc.ambient.y*2,minamb),			_max(envdesc.ambient.z*2,minamb),	0	};
					ambclr.mul		(ps_r2_sun_lumscale_amb);
		Fvector4	envclr			= { envdesc.sky_color.x*2+EPS,	envdesc.sky_color.y*2+EPS,	envdesc.sky_color.z*2+EPS,	envdesc.weight					};
		Fvector4	fogclr			= { envdesc.fog_color.x,	envdesc.fog_color.y,	envdesc.fog_color.z,		0	};
					envclr.x		*= 2*(ps_r2_sun_lumscale_hemi); 
					envclr.y		*= 2*(ps_r2_sun_lumscale_hemi); 
					envclr.z		*= 2*(ps_r2_sun_lumscale_hemi);
		Fvector4	sunclr,sundir;

		// sun-params
		{
			light*		fuckingsun		= (light*)RImplementation.Lights.sun_adapted._get()	;
			Fvector		L_dir,L_clr;	float L_spec;
			L_clr.set					(fuckingsun->color.r,fuckingsun->color.g,fuckingsun->color.b);
			L_spec						= u_diffuse2s	(L_clr);
			Device.mView.transform_dir	(L_dir,fuckingsun->direction);
			L_dir.normalize				();

			sunclr.set				(L_clr.x,L_clr.y,L_clr.z,L_spec);
			sundir.set				(L_dir.x,L_dir.y,L_dir.z,0);
		}

		// Fill VB
		float	_w					= float(Device.dwWidth);
		float	_h					= float(Device.dwHeight);
/*		if (ps_r2_pp_flags.test(R2PP_FLAG_SUPERSAMPLING_AA)) 
		{
			_w *= 2; _h *= 2;
		}*/
		p0.set						(.5f/_w, .5f/_h);
		p1.set						((_w+.5f)/_w, (_h+.5f)/_h );

		// Fill vertex buffer
		Fvector4* pv				= (Fvector4*)	RCache.Vertex.Lock	(4,g_combine_VP->vb_stride,Offset);
		pv->set						(hclip(EPS,		_w),	hclip(_h+EPS,	_h),	p0.x, p1.y);	pv++;
		pv->set						(hclip(EPS,		_w),	hclip(EPS,		_h),	p0.x, p0.y);	pv++;
		pv->set						(hclip(_w+EPS,	_w),	hclip(_h+EPS,	_h),	p1.x, p1.y);	pv++;
		pv->set						(hclip(_w+EPS,	_w),	hclip(EPS,		_h),	p1.x, p0.y);	pv++;
		RCache.Vertex.Unlock		(4,g_combine_VP->vb_stride);

		// Setup textures
		IDirect3DBaseTexture9*	e0	= _menu_pp?0:envdesc.sky_r_textures_env[0].second->surface_get();
		IDirect3DBaseTexture9*	e1	= _menu_pp?0:envdesc.sky_r_textures_env[1].second->surface_get();
		t_envmap_0->surface_set		(e0);	_RELEASE(e0);
		t_envmap_1->surface_set		(e1);	_RELEASE(e1);
	
		// Draw
		RCache.set_Element			(s_combine->E[0]	);
		RCache.set_Geometry			(g_combine_VP		);

		//doesn't belong this
//		RCache.set_c				("c_timers",	rain_map_timers->timer.x, rain_map_timers->timer.y, rain_map_timers->timer.z, envdesc.rain_density);

		RCache.set_c				("m_v2w",			m_v2w	);
		RCache.set_c				("L_ambient",		ambclr	);

		RCache.set_c				("Ldynamic_color",	sunclr	);
		RCache.set_c				("Ldynamic_dir",	sundir	);

		RCache.set_c				("env_color",		envclr	);
		RCache.set_c				("fog_color",		fogclr	);
		RCache.Render				(D3DPT_TRIANGLELIST,Offset,0,4,0,2);
	}

	if (!_menu_pp && R2RM_NORMAL==ps_Render_mode && ps_r2_ls_flags.test(R2FLAG_PUDDLES) && Puddles->m_bLoaded)				phase_puddles		();
	
	// Forward rendering
	{
		u_setrt							(rt_Generic_0,0,0,HW.pBaseZB);		// LDR RT
		RCache.set_CullMode				(CULL_CCW);
		RCache.set_Stencil				(FALSE);
		RCache.set_ColorWriteEnable		();
		g_pGamePersistent->Environment().RenderClouds	();
		RImplementation.w_render_forward	();
		u_setrt							(rt_Generic_0,0,0,HW.pBaseZB);		// LDR RT
		if (g_pGamePersistent)	g_pGamePersistent->OnRenderPPUI_main()	;	// PP-UI
	}
	// sunshafts combine
	if (!_menu_pp) phase_combine_volumetric();

	// Perform blooming filter and distortion if needed
	RCache.set_Stencil	(FALSE);
	w_phase_bloom			( );												// HDR RT invalidated here

	// Distortion filter
	BOOL	bDistort	= RImplementation.o.distortion_enabled;				// This can be modified
	{
		if		((0==RImplementation.mapDistort.size()) && !_menu_pp)		bDistort= FALSE;
		if (bDistort)		{
			u_setrt						(rt_Generic_1,0,0,HW.pBaseZB);		// Now RT is a distortion mask
			RCache.set_CullMode			(CULL_CCW);
			RCache.set_Stencil			(FALSE);
			RCache.set_ColorWriteEnable	();
			CHK_DX(HW.pDevice->Clear	( 0L, NULL, D3DCLEAR_TARGET, color_rgba(127,127,0,127), 1.0f, 0L));
			RImplementation.r_dsgraph_render_distort	();
			if (g_pGamePersistent)	g_pGamePersistent->OnRenderPPUI_PP()	;	// PP-UI
		}
	}

	// PP enabled ?
	BOOL	PP_Complex		= w_u_need_PP	();
	if (_menu_pp)			PP_Complex	= FALSE;

	// KD: bunch of posteffects.
	if (R2RM_NORMAL==ps_Render_mode) 
	{
		if (!_menu_pp)
		{
			if (ps_r2_ls_flags.test(R2FLAG_FOG_VOLUME) && FV->m_bLoaded)				phase_fog_volumes	();
//			if (ps_r2_ls_flags.test(R2FLAG_PUDDLES) && Puddles->m_bLoaded)				phase_puddles		();
//			if (ps_r2_pp_flags.test(R2PP_FLAG_SUPERSAMPLING_AA))	phase_downsample	();
			// screen space "volume" effects
			if (ps_r2_pp_flags.test(R2PP_FLAG_AA))						phase_aa				();
																		phase_wet_reflections	();
			if (ps_r2_pp_flags.test(R2PP_FLAG_SUNSHAFTS) 
				&& (ps_sunshafts_mode == R2SS_SCREEN_SPACE))			phase_sunshafts			();
																		phase_blur				();
			if (ps_r2_pp_flags.test(R2PP_FLAG_DOF))						phase_dof				();
			if (ps_r2_pp_flags.test(R2PP_FLAG_MBLUR))					phase_motion_blur		();
			// screen space "eyes" effects
			if (ps_r2_pp_flags.test(R2PP_FLAG_RAIN_DROPS) 
				&& ps_r2_pp_flags.test(R2PP_FLAG_RAIN_DROPS_CONTROL))	phase_rain_drops();
		}
	} 
	else if (R2RM_THERMAL==ps_Render_mode)
	{
		phase_thermal_vision();
		//	PP-if required
		if (PP_Complex)		{
//			w_phase_pp		();
		}

		//	Re-adapt luminance
		RCache.set_Stencil		(FALSE);
		
		//*** exposure-pipeline-clear
		{
			std::swap					(rt_LUM_pool[gpu_id*2+0],rt_LUM_pool[gpu_id*2+1]);
			t_LUM_src->surface_set		(NULL);
			t_LUM_dest->surface_set		(NULL);
		}
		return;
	}

	// Combine everything + perform AA
	if		(PP_Complex)	u_setrt		( rt_Color,0,0,HW.pBaseZB );			// LDR RT
	else					u_setrt		( Device.dwWidth,Device.dwHeight,HW.pBaseRT,NULL,NULL,HW.pBaseZB);
	//. u_setrt				( Device.dwWidth,Device.dwHeight,HW.pBaseRT,NULL,NULL,HW.pBaseZB);
	RCache.set_CullMode		( CULL_NONE )	;
	RCache.set_Stencil		( FALSE		)	;
	if (1)	
	{
		// 
		struct v_aa	{
			Fvector4	p;
			Fvector2	uv0;
			Fvector2	uv1;
			Fvector2	uv2;
			Fvector2	uv3;
			Fvector2	uv4;
			Fvector4	uv5;
			Fvector4	uv6;
		};

		float	_w					= float(Device.dwWidth);
		float	_h					= float(Device.dwHeight);
		float	ddw					= 1.f/_w;
		float	ddh					= 1.f/_h;
		p0.set						(.5f/_w, .5f/_h);
		p1.set						((_w+.5f)/_w, (_h+.5f)/_h );

		// Fill vertex buffer
		v_aa* pv					= (v_aa*) RCache.Vertex.Lock	(4,g_aa_AA->vb_stride,Offset);
		pv->p.set(EPS,			float(_h+EPS),	EPS,1.f); pv->uv0.set(p0.x, p1.y);pv->uv1.set(p0.x-ddw,p1.y-ddh);pv->uv2.set(p0.x+ddw,p1.y+ddh);pv->uv3.set(p0.x+ddw,p1.y-ddh);pv->uv4.set(p0.x-ddw,p1.y+ddh);pv->uv5.set(p0.x-ddw,p1.y,p1.y,p0.x+ddw);pv->uv6.set(p0.x,p1.y-ddh,p1.y+ddh,p0.x);pv++;
		pv->p.set(EPS,			EPS,			EPS,1.f); pv->uv0.set(p0.x, p0.y);pv->uv1.set(p0.x-ddw,p0.y-ddh);pv->uv2.set(p0.x+ddw,p0.y+ddh);pv->uv3.set(p0.x+ddw,p0.y-ddh);pv->uv4.set(p0.x-ddw,p0.y+ddh);pv->uv5.set(p0.x-ddw,p0.y,p0.y,p0.x+ddw);pv->uv6.set(p0.x,p0.y-ddh,p0.y+ddh,p0.x);pv++;
		pv->p.set(float(_w+EPS),float(_h+EPS),	EPS,1.f); pv->uv0.set(p1.x, p1.y);pv->uv1.set(p1.x-ddw,p1.y-ddh);pv->uv2.set(p1.x+ddw,p1.y+ddh);pv->uv3.set(p1.x+ddw,p1.y-ddh);pv->uv4.set(p1.x-ddw,p1.y+ddh);pv->uv5.set(p1.x-ddw,p1.y,p1.y,p1.x+ddw);pv->uv6.set(p1.x,p1.y-ddh,p1.y+ddh,p1.x);pv++;
		pv->p.set(float(_w+EPS),EPS,			EPS,1.f); pv->uv0.set(p1.x, p0.y);pv->uv1.set(p1.x-ddw,p0.y-ddh);pv->uv2.set(p1.x+ddw,p0.y+ddh);pv->uv3.set(p1.x+ddw,p0.y-ddh);pv->uv4.set(p1.x-ddw,p0.y+ddh);pv->uv5.set(p1.x-ddw,p0.y,p0.y,p1.x+ddw);pv->uv6.set(p1.x,p0.y-ddh,p0.y+ddh,p1.x);pv++;
		RCache.Vertex.Unlock		(4,g_aa_AA->vb_stride);

		// Draw COLOR
		RCache.set_Element			(s_combine->E[bDistort?4:2]);	// look at blender_combine.cpp
		RCache.set_c				("c_color_grading",	ps_r2_color_grading_params);
		RCache.set_Geometry			(g_aa_AA);
		RCache.Render				(D3DPT_TRIANGLELIST,Offset,0,4,0,2);
	}
	RCache.set_Stencil		(FALSE);

	//	if FP16-BLEND !not! supported - draw flares here, overwise they are already in the bloom target
	// if (!RImplementation.o.fp16_blend)	
	g_pGamePersistent->Environment().RenderFlares	();	// lens-flares

	//	PP-if required
	if (PP_Complex)		{
		w_phase_pp		();
	}

	//	Re-adapt luminance
	RCache.set_Stencil		(FALSE);
	
	//*** exposure-pipeline-clear
	{
		std::swap					(rt_LUM_pool[gpu_id*2+0],rt_LUM_pool[gpu_id*2+1]);
		t_LUM_src->surface_set		(NULL);
		t_LUM_dest->surface_set		(NULL);
	}

	// очистка рендертаргетов
	if (ps_r2_ls_flags.test(R2FLAG_LENS_FLARES) || ps_r2_ls_flags.test(R2FLAG_LENS_DIRT) 
		|| (ps_r2_pp_flags.test(R2PP_FLAG_SUNSHAFTS) && (ps_sunshafts_mode == R2SS_VOLUMETRIC))
		|| ps_r2_pp_flags.test(R2FLAG_VOLLIGHT))
	{
/*		u_setrt								(rt_flares,		NULL,NULL,HW.pBaseZB);
		u32		clr4clear					= color_rgba(0,0,0,0);	// 0x00
		CHK_DX	(HW.pDevice->Clear			( 0L, NULL, D3DCLEAR_TARGET, clr4clear, 1.0f, 0L));
		u_setrt		( Device.dwWidth,Device.dwHeight,HW.pBaseRT,NULL,NULL,HW.pBaseZB);*/

		if (ps_r2_ls_flags.test(R2FLAG_LENS_FLARES) || ps_r2_ls_flags.test(R2FLAG_LENS_DIRT))
			phase_flares();
		if ((ps_r2_pp_flags.test(R2PP_FLAG_SUNSHAFTS) && (ps_sunshafts_mode == R2SS_VOLUMETRIC)) || ps_r2_pp_flags.test(R2FLAG_VOLLIGHT))
			phase_accumulator_volumetric();

		u_setrt		( Device.dwWidth,Device.dwHeight,HW.pBaseRT,NULL,NULL,HW.pBaseZB);
	}
	END_PERF_EVENT;
};

void	CRenderTarget::phase_combine_volumetric	()
{
//	return;		// blocked
	if ((ps_r2_pp_flags.test(R2PP_FLAG_SUNSHAFTS) && (ps_sunshafts_mode == R2SS_VOLUMETRIC)) || (ps_r2_ls_flags.test(R2FLAG_VOLLIGHT)))
	{
		CEnvDescriptorMixer *env = &(g_pGamePersistent->Environment().CurrentEnv);
		if (env->params_ex->sun_shafts <= 0.001) return;
//		if (!is_there_sun()) return;

		u32			Offset					= 0;
		Fvector2	p0,p1;

		u_setrt(rt_Generic_0, rt_Generic_1, 0, HW.pBaseZB);
		RCache.set_ColorWriteEnable	(7);

		// Compute params
		Fmatrix		m_v2w;			m_v2w.invert				(Device.mView		);
		CEnvDescriptorMixer& envdesc= g_pGamePersistent->Environment().CurrentEnv		;
		const float minamb			= 0.001f;
		Fvector4	ambclr			= { _max(envdesc.ambient.x*2,minamb),	_max(envdesc.ambient.y*2,minamb),			_max(envdesc.ambient.z*2,minamb),	0	};
					ambclr.mul		(ps_r2_sun_lumscale_amb);
		Fvector4	envclr			= { envdesc.sky_color.x*2+EPS,	envdesc.sky_color.y*2+EPS,	envdesc.sky_color.z*2+EPS,	envdesc.weight					};
		Fvector4	fogclr			= { envdesc.fog_color.x,	envdesc.fog_color.y,	envdesc.fog_color.z,		0	};
					envclr.x		*= 2*(ps_r2_sun_lumscale_hemi); 
					envclr.y		*= 2*(ps_r2_sun_lumscale_hemi); 
					envclr.z		*= 2*(ps_r2_sun_lumscale_hemi);
		Fvector4	sunclr,sundir;

		// sun-params
		{
			light*		fuckingsun		= (light*)RImplementation.Lights.sun_adapted._get()	;
			Fvector		L_dir,L_clr;	float L_spec;
			L_clr.set					(fuckingsun->color.r,fuckingsun->color.g,fuckingsun->color.b);
			L_spec						= u_diffuse2s	(L_clr);
			Device.mView.transform_dir	(L_dir,fuckingsun->direction);
			L_dir.normalize				();

			sunclr.set				(L_clr.x,L_clr.y,L_clr.z,L_spec);
			sundir.set				(L_dir.x,L_dir.y,L_dir.z,0);
		}

		// Fill VB
		float	_w					= float(Device.dwWidth);
		float	_h					= float(Device.dwHeight);

		p0.set						(.5f/_w, .5f/_h);
		p1.set						((_w+.5f)/_w, (_h+.5f)/_h );

		// Fill vertex buffer
		Fvector4* pv				= (Fvector4*)	RCache.Vertex.Lock	(4,g_combine_VP->vb_stride,Offset);
		pv->set						(hclip(EPS,		_w),	hclip(_h+EPS,	_h),	p0.x, p1.y);	pv++;
		pv->set						(hclip(EPS,		_w),	hclip(EPS,		_h),	p0.x, p0.y);	pv++;
		pv->set						(hclip(_w+EPS,	_w),	hclip(_h+EPS,	_h),	p1.x, p1.y);	pv++;
		pv->set						(hclip(_w+EPS,	_w),	hclip(EPS,		_h),	p1.x, p0.y);	pv++;
		RCache.Vertex.Unlock		(4,g_combine_VP->vb_stride);

		// Setup textures
	/*	IDirect3DBaseTexture9*	e0	= envdesc.sky_r_textures_env[0].second->surface_get();
		IDirect3DBaseTexture9*	e1	= envdesc.sky_r_textures_env[1].second->surface_get();
		t_envmap_0->surface_set		(e0);	_RELEASE(e0);
		t_envmap_1->surface_set		(e1);	_RELEASE(e1);*/
		
		// Draw
		RCache.set_Shader			(s_combine_volumetric);
		RCache.set_Geometry			(g_combine_VP		);

		RCache.set_c				("m_v2w",			m_v2w	);
		RCache.set_c				("L_ambient",		ambclr	);

		RCache.set_c				("Ldynamic_color",	sunclr	);
		RCache.set_c				("Ldynamic_dir",	sundir	);

		RCache.set_c				("env_color",		envclr	);
//		RCache.set_c				("fog_color",		fogclr	);
		RCache.Render				(D3DPT_TRIANGLELIST,Offset,0,4,0,2);

		RCache.set_ColorWriteEnable	(15);
	}
}