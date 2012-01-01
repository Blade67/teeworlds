/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include <base/detect.h>
#include <base/math.h>

#include "SDL.h"
#include "SDL_opengl.h"

#include <base/system.h>
#include <engine/external/pnglite/pnglite.h>

#include <engine/shared/config.h>
#include <engine/graphics.h>
#include <engine/storage.h>
#include <engine/keys.h>
#include <engine/console.h>

#include <math.h> // cosf, sinf

#include "graphics_threaded.h"


static CVideoMode g_aFakeModes[] = {
	{320,240,8,8,8}, {400,300,8,8,8}, {640,480,8,8,8},
	{720,400,8,8,8}, {768,576,8,8,8}, {800,600,8,8,8},
	{1024,600,8,8,8}, {1024,768,8,8,8}, {1152,864,8,8,8},
	{1280,768,8,8,8}, {1280,800,8,8,8}, {1280,960,8,8,8},
	{1280,1024,8,8,8}, {1368,768,8,8,8}, {1400,1050,8,8,8},
	{1440,900,8,8,8}, {1440,1050,8,8,8}, {1600,1000,8,8,8},
	{1600,1200,8,8,8}, {1680,1050,8,8,8}, {1792,1344,8,8,8},
	{1800,1440,8,8,8}, {1856,1392,8,8,8}, {1920,1080,8,8,8},
	{1920,1200,8,8,8}, {1920,1440,8,8,8}, {1920,2400,8,8,8},
	{2048,1536,8,8,8},

	{320,240,5,6,5}, {400,300,5,6,5}, {640,480,5,6,5},
	{720,400,5,6,5}, {768,576,5,6,5}, {800,600,5,6,5},
	{1024,600,5,6,5}, {1024,768,5,6,5}, {1152,864,5,6,5},
	{1280,768,5,6,5}, {1280,800,5,6,5}, {1280,960,5,6,5},
	{1280,1024,5,6,5}, {1368,768,5,6,5}, {1400,1050,5,6,5},
	{1440,900,5,6,5}, {1440,1050,5,6,5}, {1600,1000,5,6,5},
	{1600,1200,5,6,5}, {1680,1050,5,6,5}, {1792,1344,5,6,5},
	{1800,1440,5,6,5}, {1856,1392,5,6,5}, {1920,1080,5,6,5},
	{1920,1200,5,6,5}, {1920,1440,5,6,5}, {1920,2400,5,6,5},
	{2048,1536,5,6,5}
};

class CCommandProcessorFragment_General
{
public:
	void Cmd_Signal(const CCommandBuffer::SCommand_Signal *pCommand)
	{
		pCommand->m_pSemaphore->signal();
	}

	bool RunCommand(const CCommandBuffer::SCommand * pBaseCommand)
	{
		switch(pBaseCommand->m_Cmd)
		{
		case CCommandBuffer::CMD_NOP: break;
		case CCommandBuffer::CMD_SIGNAL: Cmd_Signal(static_cast<const CCommandBuffer::SCommand_Signal *>(pBaseCommand)); break;
		default: return false;
		}

		return true;
	}
};

class CCommandProcessorFragment_OpenGL
{
	GLuint m_aTextures[CCommandBuffer::MAX_TEXTURES];

	void SetState(const CCommandBuffer::SState &State)
	{
		// blend
		switch(State.m_BlendMode)
		{
		case CCommandBuffer::BLEND_NONE:
			glDisable(GL_BLEND);
			break;
		case CCommandBuffer::BLEND_ALPHA:
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			break;
		case CCommandBuffer::BLEND_ADDITIVE:
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE);
			break;
		default:
			dbg_msg("render", "unknown blendmode %d\n", State.m_BlendMode);
		};

		// clip
		if(State.m_ClipEnable)
		{
			glScissor(State.m_ClipX, State.m_ClipY, State.m_ClipW, State.m_ClipH);
			glEnable(GL_SCISSOR_TEST);
		}
		else
			glDisable(GL_SCISSOR_TEST);
		
		// texture
		if(State.m_Texture >= 0 && State.m_Texture < CCommandBuffer::MAX_TEXTURES)
		{
			glEnable(GL_TEXTURE_2D);
			glBindTexture(GL_TEXTURE_2D, m_aTextures[State.m_Texture]);
		}
		else
			glDisable(GL_TEXTURE_2D);

		// screen mapping
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glOrtho(State.m_ScreenTL.x, State.m_ScreenBR.x, State.m_ScreenBR.y, State.m_ScreenTL.y, 1.0f, 10.f);
	}

	static int TexFormatToOpenGLFormat(int TexFormat)
	{
		if(TexFormat == CCommandBuffer::TEXFORMAT_RGB) return GL_RGB;
		if(TexFormat == CCommandBuffer::TEXFORMAT_ALPHA) return GL_ALPHA;
		if(TexFormat == CCommandBuffer::TEXFORMAT_RGBA) return GL_RGBA;
		return GL_RGBA;
	}

	void Cmd_Texture_Update(const CCommandBuffer::SCommand_Texture_Update *pCommand)
	{
		glBindTexture(GL_TEXTURE_2D, m_aTextures[pCommand->m_Slot]);
		glTexSubImage2D(GL_TEXTURE_2D, 0, pCommand->m_X, pCommand->m_Y, pCommand->m_Width, pCommand->m_Height,
			TexFormatToOpenGLFormat(pCommand->m_Format), GL_UNSIGNED_BYTE, pCommand->m_pData);
		mem_free(pCommand->m_pData);
	}

	void Cmd_Texture_Destroy(const CCommandBuffer::SCommand_Texture_Destroy *pCommand)
	{
		glDeleteTextures(1, &m_aTextures[pCommand->m_Slot]);
	}

	void Cmd_Texture_Create(const CCommandBuffer::SCommand_Texture_Create *pCommand)
	{
		int Oglformat = TexFormatToOpenGLFormat(pCommand->m_Format);

		// upload texture
		int StoreOglformat = Oglformat;
		if(g_Config.m_GfxTextureCompression)
		{
			StoreOglformat = GL_COMPRESSED_RGBA_ARB;
			if(pCommand->m_StoreFormat == CCommandBuffer::TEXFORMAT_RGB)
				StoreOglformat = GL_COMPRESSED_RGB_ARB;
			else if(Oglformat == CCommandBuffer::TEXFORMAT_ALPHA)
				StoreOglformat = GL_COMPRESSED_ALPHA_ARB;
		}
		else
		{
			StoreOglformat = TexFormatToOpenGLFormat(pCommand->m_StoreFormat);
		}

		glGenTextures(1, &m_aTextures[pCommand->m_Slot]);
		glBindTexture(GL_TEXTURE_2D, m_aTextures[pCommand->m_Slot]);

		if(pCommand->m_Flags&CCommandBuffer::TEXFLAG_NOMIPMAPS)
		{
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexImage2D(GL_TEXTURE_2D, 0, StoreOglformat, pCommand->m_Width, pCommand->m_Height, 0, Oglformat, GL_UNSIGNED_BYTE, pCommand->m_pData);
		}
		else
		{
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
			gluBuild2DMipmaps(GL_TEXTURE_2D, StoreOglformat, pCommand->m_Width, pCommand->m_Height, Oglformat, GL_UNSIGNED_BYTE, pCommand->m_pData);
		}

		mem_free(pCommand->m_pData);
	}

	void Cmd_Clear(const CCommandBuffer::SCommand_Clear *pCommand)
	{
		glClearColor(pCommand->m_Color.r, pCommand->m_Color.g, pCommand->m_Color.b, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	}

	void Cmd_Render(const CCommandBuffer::SCommand_Render *pCommand)
	{
		SetState(pCommand->m_State);
		
		glVertexPointer(3, GL_FLOAT, sizeof(CCommandBuffer::SVertex), (char*)pCommand->m_pVertices);
		glTexCoordPointer(2, GL_FLOAT, sizeof(CCommandBuffer::SVertex), (char*)pCommand->m_pVertices + sizeof(float)*3);
		glColorPointer(4, GL_FLOAT, sizeof(CCommandBuffer::SVertex), (char*)pCommand->m_pVertices + sizeof(float)*5);
		glEnableClientState(GL_VERTEX_ARRAY);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glEnableClientState(GL_COLOR_ARRAY);

		switch(pCommand->m_PrimType)
		{
		case CCommandBuffer::PRIMTYPE_QUADS:
			glDrawArrays(GL_QUADS, 0, pCommand->m_PrimCount*4);
			break;
		case CCommandBuffer::PRIMTYPE_LINES:
			glDrawArrays(GL_LINES, 0, pCommand->m_PrimCount*2);
			break;
		default:
			dbg_msg("render", "unknown primtype %d\n", pCommand->m_Cmd);
		};
	}

	void Cmd_Screenshot(const CCommandBuffer::SCommand_Screenshot *pCommand)
	{
		// fetch image data
		GLint aViewport[4] = {0,0,0,0};
		glGetIntegerv(GL_VIEWPORT, aViewport);

		int w = aViewport[2];
		int h = aViewport[3];

		dbg_msg("graphics", "grabbing %d x %d", w, h);

		// we allocate one more row to use when we are flipping the texture
		unsigned char *pPixelData = (unsigned char *)mem_alloc(w*(h+1)*3, 1);
		unsigned char *pTempRow = pPixelData+w*h*3;

		// fetch the pixels
		GLint Alignment;
		glGetIntegerv(GL_PACK_ALIGNMENT, &Alignment);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadPixels(0,0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pPixelData);
		glPixelStorei(GL_PACK_ALIGNMENT, Alignment);

		// flip the pixel because opengl works from bottom left corner
		for(int y = 0; y < h/2; y++)
		{
			mem_copy(pTempRow, pPixelData+y*w*3, w*3);
			mem_copy(pPixelData+y*w*3, pPixelData+(h-y-1)*w*3, w*3);
			mem_copy(pPixelData+(h-y-1)*w*3, pTempRow,w*3);
		}

		// fill in the information
		pCommand->m_pImage->m_Width = w;
		pCommand->m_pImage->m_Height = h;
		pCommand->m_pImage->m_Format = CImageInfo::FORMAT_RGB;
		pCommand->m_pImage->m_pData = pPixelData;
	}

public:
	CCommandProcessorFragment_OpenGL()
	{
		mem_zero(m_aTextures, sizeof(m_aTextures));
	}

	bool RunCommand(const CCommandBuffer::SCommand * pBaseCommand)
	{
		switch(pBaseCommand->m_Cmd)
		{
		case CCommandBuffer::CMD_TEXTURE_CREATE: Cmd_Texture_Create(static_cast<const CCommandBuffer::SCommand_Texture_Create *>(pBaseCommand)); break;
		case CCommandBuffer::CMD_TEXTURE_DESTROY: Cmd_Texture_Destroy(static_cast<const CCommandBuffer::SCommand_Texture_Destroy *>(pBaseCommand)); break;
		case CCommandBuffer::CMD_TEXTURE_UPDATE: Cmd_Texture_Update(static_cast<const CCommandBuffer::SCommand_Texture_Update *>(pBaseCommand)); break;
		case CCommandBuffer::CMD_CLEAR: Cmd_Clear(static_cast<const CCommandBuffer::SCommand_Clear *>(pBaseCommand)); break;
		case CCommandBuffer::CMD_RENDER: Cmd_Render(static_cast<const CCommandBuffer::SCommand_Render *>(pBaseCommand)); break;
		case CCommandBuffer::CMD_SCREENSHOT: Cmd_Screenshot(static_cast<const CCommandBuffer::SCommand_Screenshot *>(pBaseCommand)); break;
		default: return false;
		}

		return true;
	}
};

class CCommandProcessorFragment_SDL
{
	// SDL stuff
	SDL_Surface *m_pScreenSurface;
	bool m_SystemInited;

	void Cmd_Init(const CCommandBuffer::SCommand_Init *pCommand)
	{
		if(!m_SystemInited)
		{
			int Systems = SDL_INIT_VIDEO;

			if(g_Config.m_SndEnable) // TODO: remove
				Systems |= SDL_INIT_AUDIO;

			if(g_Config.m_ClEventthread) // TODO: remove
				Systems |= SDL_INIT_EVENTTHREAD;

			if(SDL_Init(Systems) < 0)
			{
				dbg_msg("gfx", "unable to init SDL: %s", SDL_GetError());
				*pCommand->m_pResult = -1;
				return;
			}

			atexit(SDL_Quit); // ignore_convention

			#ifdef CONF_FAMILY_WINDOWS
				if(!getenv("SDL_VIDEO_WINDOW_POS") && !getenv("SDL_VIDEO_CENTERED")) // ignore_convention
					putenv("SDL_VIDEO_WINDOW_POS=8,27"); // ignore_convention
			#endif
			
			m_SystemInited = true;
		}

		const SDL_VideoInfo *pInfo = SDL_GetVideoInfo();
		SDL_EventState(SDL_MOUSEMOTION, SDL_IGNORE);

		// set flags
		int Flags = SDL_OPENGL;
		if(pCommand->m_Flags&CCommandBuffer::INITFLAG_RESIZABLE)
			Flags |= SDL_RESIZABLE;

		if(pInfo->hw_available) // ignore_convention
			Flags |= SDL_HWSURFACE;
		else
			Flags |= SDL_SWSURFACE;

		if(pInfo->blit_hw) // ignore_convention
			Flags |= SDL_HWACCEL;

		if(pCommand->m_Flags&CCommandBuffer::INITFLAG_FULLSCREEN)
			Flags |= SDL_FULLSCREEN;

		// set gl attributes
		if(pCommand->m_FsaaSamples)
		{
			SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
			SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, pCommand->m_FsaaSamples);
		}
		else
		{
			SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
			SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
		}

		SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
		SDL_GL_SetAttribute(SDL_GL_SWAP_CONTROL, pCommand->m_Flags&CCommandBuffer::INITFLAG_VSYNC ? 1 : 0);

		// set caption
		SDL_WM_SetCaption(pCommand->m_aName, pCommand->m_aName);

		// create window
		m_pScreenSurface = SDL_SetVideoMode(pCommand->m_ScreenWidth, pCommand->m_ScreenHeight, 0, Flags);
		if(m_pScreenSurface)
		{
			SDL_ShowCursor(0);

			// set some default settings
			glEnable(GL_BLEND);
			glDisable(GL_CULL_FACE);
			glDisable(GL_DEPTH_TEST);
			glMatrixMode(GL_MODELVIEW);
			glLoadIdentity();

			glAlphaFunc(GL_GREATER, 0);
			glEnable(GL_ALPHA_TEST);
			glDepthMask(0);

			*pCommand->m_pResult = 0;
		}
		else
		{
			dbg_msg("gfx", "unable to set video mode: %s", SDL_GetError());
			*pCommand->m_pResult = -1;
		}
	}

	void Cmd_Shutdown(const CCommandBuffer::SCommand_Shutdown *pCommand)
	{
		SDL_Quit();
	}

	void Cmd_Swap(const CCommandBuffer::SCommand_Swap *pCommand)
	{
		SDL_GL_SwapBuffers();
	}

public:
	CCommandProcessorFragment_SDL()
	{
		m_SystemInited = false;
		m_pScreenSurface = 0x0;
	}

	bool RunCommand(const CCommandBuffer::SCommand *pBaseCommand)
	{
		switch(pBaseCommand->m_Cmd)
		{
		case CCommandBuffer::CMD_INIT: Cmd_Init(static_cast<const CCommandBuffer::SCommand_Init *>(pBaseCommand)); break;
		case CCommandBuffer::CMD_SHUTDOWN: Cmd_Shutdown(static_cast<const CCommandBuffer::SCommand_Shutdown *>(pBaseCommand)); break;
		case CCommandBuffer::CMD_SWAP: Cmd_Swap(static_cast<const CCommandBuffer::SCommand_Swap *>(pBaseCommand)); break;
		default: return false;
		}

		return true;
	}
};

class CCommandProcessor_SDL_OpenGL : public ICommandProcessor
{
 	CCommandProcessorFragment_OpenGL m_OpenGL;
 	CCommandProcessorFragment_SDL m_SDL;
 	CCommandProcessorFragment_General m_General;
 public:
	virtual void RunBuffer(CCommandBuffer *pBuffer)
	{
		unsigned CmdIndex = 0;
		while(1)
		{
			const CCommandBuffer::SCommand *pBaseCommand = pBuffer->GetCommand(&CmdIndex);
			if(pBaseCommand == 0x0)
				break;
			
			if(m_OpenGL.RunCommand(pBaseCommand))
				continue;
			
			if(m_SDL.RunCommand(pBaseCommand))
				continue;

			if(m_General.RunCommand(pBaseCommand))
				continue;
			
			dbg_msg("graphics", "unknown command %d", pBaseCommand->m_Cmd);
		}
	}
};

void CCommandProcessorHandler::ThreadFunc(void *pUser)
{
	CCommandProcessorHandler *pThis = (CCommandProcessorHandler *)pUser;

	while(!pThis->m_Shutdown)
	{
		pThis->m_Activity.wait();
		if(pThis->m_pBuffer)
		{
			pThis->m_pProcessor->RunBuffer(pThis->m_pBuffer);
			sync_barrier();
			pThis->m_pBuffer = 0x0;
			pThis->m_BufferDone.signal();
		}
	}
}

CCommandProcessorHandler::CCommandProcessorHandler()
{
	m_pBuffer = 0x0;
	m_pProcessor = 0x0;
	m_pThread = 0x0;
}

void CCommandProcessorHandler::Start(ICommandProcessor *pProcessor)
{
	m_Shutdown = false;
	m_pProcessor = pProcessor;
	m_pThread = thread_create(ThreadFunc, this);
	m_BufferDone.signal();
}

void CCommandProcessorHandler::Stop()
{
	m_Shutdown = true;
	m_Activity.signal();
	thread_wait(m_pThread);
	thread_destroy(m_pThread);
}

void CCommandProcessorHandler::RunBuffer(CCommandBuffer *pBuffer)
{
	WaitForIdle();
	m_pBuffer = pBuffer;
	m_Activity.signal();
}

void CCommandProcessorHandler::WaitForIdle()
{
	while(m_pBuffer != 0x0)
		m_BufferDone.wait();
}

void CGraphics_Threaded::FlushVertices()
{
	if(m_NumVertices == 0)
		return;

	int NumVerts = m_NumVertices;
	m_NumVertices = 0;

	CCommandBuffer::SCommand_Render Cmd;
	Cmd.m_State = m_State;

	if(m_Drawing == DRAWING_QUADS)
	{
		Cmd.m_PrimType = CCommandBuffer::PRIMTYPE_QUADS;
		Cmd.m_PrimCount = NumVerts/4;
	}
	else if(m_Drawing == DRAWING_LINES)
	{
		Cmd.m_PrimType = CCommandBuffer::PRIMTYPE_LINES;
		Cmd.m_PrimCount = NumVerts/2;
	}
	else
		return;

	Cmd.m_pVertices = (CCommandBuffer::SVertex *)m_pCommandBuffer->AllocData(sizeof(CCommandBuffer::SVertex)*NumVerts);
	if(Cmd.m_pVertices == 0x0)
		return;

	mem_copy(Cmd.m_pVertices, m_aVertices, sizeof(CCommandBuffer::SVertex)*NumVerts);
	m_pCommandBuffer->AddCommand(Cmd);
}

void CGraphics_Threaded::AddVertices(int Count)
{
	m_NumVertices += Count;
	if((m_NumVertices + Count) >= MAX_VERTICES)
		FlushVertices();
}

void CGraphics_Threaded::Rotate4(const CCommandBuffer::SPoint &rCenter, CCommandBuffer::SVertex *pPoints)
{
	float c = cosf(m_Rotation);
	float s = sinf(m_Rotation);
	float x, y;
	int i;

	for(i = 0; i < 4; i++)
	{
		x = pPoints[i].m_Pos.x - rCenter.x;
		y = pPoints[i].m_Pos.y - rCenter.y;
		pPoints[i].m_Pos.x = x * c - y * s + rCenter.x;
		pPoints[i].m_Pos.y = x * s + y * c + rCenter.y;
	}
}

unsigned char CGraphics_Threaded::Sample(int w, int h, const unsigned char *pData, int u, int v, int Offset, int ScaleW, int ScaleH, int Bpp)
{
	int Value = 0;
	for(int x = 0; x < ScaleW; x++)
		for(int y = 0; y < ScaleH; y++)
			Value += pData[((v+y)*w+(u+x))*Bpp+Offset];
	return Value/(ScaleW*ScaleH);
}

unsigned char *CGraphics_Threaded::Rescale(int Width, int Height, int NewWidth, int NewHeight, int Format, const unsigned char *pData)
{
	unsigned char *pTmpData;
	int ScaleW = Width/NewWidth;
	int ScaleH = Height/NewHeight;

	int Bpp = 3;
	if(Format == CImageInfo::FORMAT_RGBA)
		Bpp = 4;

	pTmpData = (unsigned char *)mem_alloc(NewWidth*NewHeight*Bpp, 1);

	int c = 0;
	for(int y = 0; y < NewHeight; y++)
		for(int x = 0; x < NewWidth; x++, c++)
		{
			pTmpData[c*Bpp] = Sample(Width, Height, pData, x*ScaleW, y*ScaleH, 0, ScaleW, ScaleH, Bpp);
			pTmpData[c*Bpp+1] = Sample(Width, Height, pData, x*ScaleW, y*ScaleH, 1, ScaleW, ScaleH, Bpp);
			pTmpData[c*Bpp+2] = Sample(Width, Height, pData, x*ScaleW, y*ScaleH, 2, ScaleW, ScaleH, Bpp);
			if(Bpp == 4)
				pTmpData[c*Bpp+3] = Sample(Width, Height, pData, x*ScaleW, y*ScaleH, 3, ScaleW, ScaleH, Bpp);
		}

	return pTmpData;
}

CGraphics_Threaded::CGraphics_Threaded()
{
	m_State.m_ScreenTL.x = 0;
	m_State.m_ScreenTL.y = 0;
	m_State.m_ScreenBR.x = 0;
	m_State.m_ScreenBR.y = 0;
	m_State.m_ClipEnable = false;
	m_State.m_ClipX = 0;
	m_State.m_ClipY = 0;
	m_State.m_ClipW = 0;
	m_State.m_ClipH = 0;
	m_State.m_Texture = -1;
	m_State.m_BlendMode = CCommandBuffer::BLEND_NONE;

	m_CurrentCommandBuffer = 0;
	m_pCommandBuffer = 0x0;
	m_apCommandBuffers[0] = 0x0;
	m_apCommandBuffers[1] = 0x0;

	m_NumVertices = 0;

	m_ScreenWidth = -1;
	m_ScreenHeight = -1;

	m_Rotation = 0;
	m_Drawing = 0;
	m_InvalidTexture = 0;

	m_TextureMemoryUsage = 0;

	m_RenderEnable = true;
	m_DoScreenshot = false;
}

void CGraphics_Threaded::ClipEnable(int x, int y, int w, int h)
{
	if(x < 0)
		w += x;
	if(y < 0)
		h += y;

	x = clamp(x, 0, ScreenWidth());
	y = clamp(y, 0, ScreenHeight());
	w = clamp(w, 0, ScreenWidth()-x);
	h = clamp(h, 0, ScreenHeight()-y);

	m_State.m_ClipEnable = true;
	m_State.m_ClipX = x;
	m_State.m_ClipY = ScreenHeight()-(y+h);
	m_State.m_ClipW = w;
	m_State.m_ClipH = h;
}

void CGraphics_Threaded::ClipDisable()
{
	m_State.m_ClipEnable = false;
}

void CGraphics_Threaded::BlendNone()
{
	m_State.m_BlendMode = CCommandBuffer::BLEND_NONE;
}

void CGraphics_Threaded::BlendNormal()
{
	m_State.m_BlendMode = CCommandBuffer::BLEND_ALPHA;
}

void CGraphics_Threaded::BlendAdditive()
{
	m_State.m_BlendMode = CCommandBuffer::BLEND_ADDITIVE;
}

int CGraphics_Threaded::MemoryUsage() const
{
	return m_TextureMemoryUsage;
}

void CGraphics_Threaded::MapScreen(float TopLeftX, float TopLeftY, float BottomRightX, float BottomRightY)
{
	m_State.m_ScreenTL.x = TopLeftX;
	m_State.m_ScreenTL.y = TopLeftY;
	m_State.m_ScreenBR.x = BottomRightX;
	m_State.m_ScreenBR.y = BottomRightY;
}

void CGraphics_Threaded::GetScreen(float *pTopLeftX, float *pTopLeftY, float *pBottomRightX, float *pBottomRightY)
{
	*pTopLeftX = m_State.m_ScreenTL.x;
	*pTopLeftY = m_State.m_ScreenTL.y;
	*pBottomRightX = m_State.m_ScreenBR.x;
	*pBottomRightY = m_State.m_ScreenBR.y;
}

void CGraphics_Threaded::LinesBegin()
{
	dbg_assert(m_Drawing == 0, "called Graphics()->LinesBegin twice");
	m_Drawing = DRAWING_LINES;
	SetColor(1,1,1,1);
}

void CGraphics_Threaded::LinesEnd()
{
	dbg_assert(m_Drawing == DRAWING_LINES, "called Graphics()->LinesEnd without begin");
	FlushVertices();
	m_Drawing = 0;
}

void CGraphics_Threaded::LinesDraw(const CLineItem *pArray, int Num)
{
	dbg_assert(m_Drawing == DRAWING_LINES, "called Graphics()->LinesDraw without begin");

	for(int i = 0; i < Num; ++i)
	{
		m_aVertices[m_NumVertices + 2*i].m_Pos.x = pArray[i].m_X0;
		m_aVertices[m_NumVertices + 2*i].m_Pos.y = pArray[i].m_Y0;
		m_aVertices[m_NumVertices + 2*i].m_Tex = m_aTexture[0];
		m_aVertices[m_NumVertices + 2*i].m_Color = m_aColor[0];

		m_aVertices[m_NumVertices + 2*i + 1].m_Pos.x = pArray[i].m_X1;
		m_aVertices[m_NumVertices + 2*i + 1].m_Pos.y = pArray[i].m_Y1;
		m_aVertices[m_NumVertices + 2*i + 1].m_Tex = m_aTexture[1];
		m_aVertices[m_NumVertices + 2*i + 1].m_Color = m_aColor[1];
	}

	AddVertices(2*Num);
}

int CGraphics_Threaded::UnloadTexture(int Index)
{
	if(Index == m_InvalidTexture)
		return 0;

	if(Index < 0)
		return 0;

	CCommandBuffer::SCommand_Texture_Destroy Cmd;
	Cmd.m_Slot = Index;
	m_pCommandBuffer->AddCommand(Cmd);

	m_aTextures[Index].m_Next = m_FirstFreeTexture;
	m_TextureMemoryUsage -= m_aTextures[Index].m_MemSize;
	m_FirstFreeTexture = Index;
	return 0;
}

static int ImageFormatToTexFormat(int Format)
{
	if(Format == CImageInfo::FORMAT_RGB) return CCommandBuffer::TEXFORMAT_RGB;
	if(Format == CImageInfo::FORMAT_RGBA) return CCommandBuffer::TEXFORMAT_RGBA;
	if(Format == CImageInfo::FORMAT_ALPHA) return CCommandBuffer::TEXFORMAT_ALPHA;
	return CCommandBuffer::TEXFORMAT_RGBA;
}


int CGraphics_Threaded::LoadTextureRawSub(int TextureID, int x, int y, int Width, int Height, int Format, const void *pData)
{
	CCommandBuffer::SCommand_Texture_Update Cmd;
	Cmd.m_Slot = TextureID;
	Cmd.m_X = x;
	Cmd.m_Y = y;
	Cmd.m_Width = Width;
	Cmd.m_Height = Height;
	Cmd.m_Format = ImageFormatToTexFormat(Format);

	// calculate memory usage
	int PixelSize = 4;
	if(Format == CImageInfo::FORMAT_RGB)
		PixelSize = 3;
	else if(Format == CImageInfo::FORMAT_ALPHA)
		PixelSize = 1;

	int MemSize = Width*Height*PixelSize;

	// copy texture data
	void *pTmpData = mem_alloc(MemSize, sizeof(void*));
	mem_copy(pTmpData, pData, MemSize);
	Cmd.m_pData = pTmpData;

	//
	m_pCommandBuffer->AddCommand(Cmd);
	return 0;
}

int CGraphics_Threaded::LoadTextureRaw(int Width, int Height, int Format, const void *pData, int StoreFormat, int Flags)
{
	// don't waste memory on texture if we are stress testing
	if(g_Config.m_DbgStress)
		return m_InvalidTexture;

	// grab texture
	int Tex = m_FirstFreeTexture;
	m_FirstFreeTexture = m_aTextures[Tex].m_Next;
	m_aTextures[Tex].m_Next = -1;

	CCommandBuffer::SCommand_Texture_Create Cmd;
	Cmd.m_Slot = Tex;
	Cmd.m_Width = Width;
	Cmd.m_Height = Height;
	Cmd.m_Format = ImageFormatToTexFormat(Format);
	Cmd.m_StoreFormat = ImageFormatToTexFormat(StoreFormat);

	// flags
	Cmd.m_Flags = 0;
	if(Flags&IGraphics::TEXLOAD_NOMIPMAPS)
		Cmd.m_Flags |= CCommandBuffer::TEXFLAG_NOMIPMAPS;

	// calculate memory usage
	int PixelSize = 4;
	if(Format == CImageInfo::FORMAT_RGB)
		PixelSize = 3;
	else if(Format == CImageInfo::FORMAT_ALPHA)
		PixelSize = 1;

	int MemSize = Width*Height*PixelSize;

	// copy texture data
	void *pTmpData = mem_alloc(MemSize, sizeof(void*));
	mem_copy(pTmpData, pData, MemSize);
	Cmd.m_pData = pTmpData;

	//
	m_pCommandBuffer->AddCommand(Cmd);

	// calculate memory usage
	int MemUsage = MemSize;
	while(Width > 2 && Height > 2)
	{
		Width>>=1;
		Height>>=1;
		MemUsage += Width*Height*PixelSize;
	}

	m_TextureMemoryUsage += MemUsage;
	//mem_free(pTmpData);
	return Tex;
}

// simple uncompressed RGBA loaders
int CGraphics_Threaded::LoadTexture(const char *pFilename, int StorageType, int StoreFormat, int Flags)
{
	int l = str_length(pFilename);
	int ID;
	CImageInfo Img;

	if(l < 3)
		return -1;
	if(LoadPNG(&Img, pFilename, StorageType))
	{
		if (StoreFormat == CImageInfo::FORMAT_AUTO)
			StoreFormat = Img.m_Format;

		ID = LoadTextureRaw(Img.m_Width, Img.m_Height, Img.m_Format, Img.m_pData, StoreFormat, Flags);
		mem_free(Img.m_pData);
		if(ID != m_InvalidTexture && g_Config.m_Debug)
			dbg_msg("graphics/texture", "loaded %s", pFilename);
		return ID;
	}

	return m_InvalidTexture;
}

int CGraphics_Threaded::LoadPNG(CImageInfo *pImg, const char *pFilename, int StorageType)
{
	char aCompleteFilename[512];
	unsigned char *pBuffer;
	png_t Png; // ignore_convention

	// open file for reading
	png_init(0,0); // ignore_convention

	IOHANDLE File = m_pStorage->OpenFile(pFilename, IOFLAG_READ, StorageType, aCompleteFilename, sizeof(aCompleteFilename));
	if(File)
		io_close(File);
	else
	{
		dbg_msg("game/png", "failed to open file. filename='%s'", pFilename);
		return 0;
	}

	int Error = png_open_file(&Png, aCompleteFilename); // ignore_convention
	if(Error != PNG_NO_ERROR)
	{
		dbg_msg("game/png", "failed to open file. filename='%s'", aCompleteFilename);
		if(Error != PNG_FILE_ERROR)
			png_close_file(&Png); // ignore_convention
		return 0;
	}

	if(Png.depth != 8 || (Png.color_type != PNG_TRUECOLOR && Png.color_type != PNG_TRUECOLOR_ALPHA)) // ignore_convention
	{
		dbg_msg("game/png", "invalid format. filename='%s'", aCompleteFilename);
		png_close_file(&Png); // ignore_convention
		return 0;
	}

	pBuffer = (unsigned char *)mem_alloc(Png.width * Png.height * Png.bpp, 1); // ignore_convention
	png_get_data(&Png, pBuffer); // ignore_convention
	png_close_file(&Png); // ignore_convention

	pImg->m_Width = Png.width; // ignore_convention
	pImg->m_Height = Png.height; // ignore_convention
	if(Png.color_type == PNG_TRUECOLOR) // ignore_convention
		pImg->m_Format = CImageInfo::FORMAT_RGB;
	else if(Png.color_type == PNG_TRUECOLOR_ALPHA) // ignore_convention
		pImg->m_Format = CImageInfo::FORMAT_RGBA;
	pImg->m_pData = pBuffer;
	return 1;
}

void CGraphics_Threaded::KickCommandBuffer()
{
	m_Handler.RunBuffer(m_pCommandBuffer);

	// swap buffer
	m_CurrentCommandBuffer ^= 1;
	m_pCommandBuffer = m_apCommandBuffers[m_CurrentCommandBuffer];
	m_pCommandBuffer->Reset();
}

void CGraphics_Threaded::ScreenshotDirect(const char *pFilename)
{
	// add swap command
	CImageInfo Image;
	mem_zero(&Image, sizeof(Image));

	CCommandBuffer::SCommand_Screenshot Cmd;
	Cmd.m_pImage = &Image;
	m_pCommandBuffer->AddCommand(Cmd);

	// kick the buffer and wait for the result
	KickCommandBuffer();
	WaitForIdle();

	if(Image.m_pData)
	{
		// find filename
		char aWholePath[1024];
		png_t Png; // ignore_convention

		IOHANDLE File = m_pStorage->OpenFile(pFilename, IOFLAG_WRITE, IStorage::TYPE_SAVE, aWholePath, sizeof(aWholePath));
		if(File)
			io_close(File);

		// save png
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "saved screenshot to '%s'", aWholePath);
		m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "client", aBuf);
		png_open_file_write(&Png, aWholePath); // ignore_convention
		png_set_data(&Png, Image.m_Width, Image.m_Height, 8, PNG_TRUECOLOR, (unsigned char *)Image.m_pData); // ignore_convention
		png_close_file(&Png); // ignore_convention

		mem_free(Image.m_pData);
	}
}

void CGraphics_Threaded::TextureSet(int TextureID)
{
	dbg_assert(m_Drawing == 0, "called Graphics()->TextureSet within begin");
	m_State.m_Texture = TextureID;
}

void CGraphics_Threaded::Clear(float r, float g, float b)
{
	CCommandBuffer::SCommand_Clear Cmd;
	Cmd.m_Color.r = r;
	Cmd.m_Color.g = g;
	Cmd.m_Color.b = b;
	Cmd.m_Color.a = 0;
	m_pCommandBuffer->AddCommand(Cmd);
}

void CGraphics_Threaded::QuadsBegin()
{
	dbg_assert(m_Drawing == 0, "called Graphics()->QuadsBegin twice");
	m_Drawing = DRAWING_QUADS;

	QuadsSetSubset(0,0,1,1);
	QuadsSetRotation(0);
	SetColor(1,1,1,1);
}

void CGraphics_Threaded::QuadsEnd()
{
	dbg_assert(m_Drawing == DRAWING_QUADS, "called Graphics()->QuadsEnd without begin");
	FlushVertices();
	m_Drawing = 0;
}

void CGraphics_Threaded::QuadsSetRotation(float Angle)
{
	dbg_assert(m_Drawing == DRAWING_QUADS, "called Graphics()->QuadsSetRotation without begin");
	m_Rotation = Angle;
}

void CGraphics_Threaded::SetColorVertex(const CColorVertex *pArray, int Num)
{
	dbg_assert(m_Drawing != 0, "called Graphics()->SetColorVertex without begin");

	for(int i = 0; i < Num; ++i)
	{
		m_aColor[pArray[i].m_Index].r = pArray[i].m_R;
		m_aColor[pArray[i].m_Index].g = pArray[i].m_G;
		m_aColor[pArray[i].m_Index].b = pArray[i].m_B;
		m_aColor[pArray[i].m_Index].a = pArray[i].m_A;
	}
}

void CGraphics_Threaded::SetColor(float r, float g, float b, float a)
{
	dbg_assert(m_Drawing != 0, "called Graphics()->SetColor without begin");
	CColorVertex Array[4] = {
		CColorVertex(0, r, g, b, a),
		CColorVertex(1, r, g, b, a),
		CColorVertex(2, r, g, b, a),
		CColorVertex(3, r, g, b, a)};
	SetColorVertex(Array, 4);
}

void CGraphics_Threaded::QuadsSetSubset(float TlU, float TlV, float BrU, float BrV)
{
	dbg_assert(m_Drawing == DRAWING_QUADS, "called Graphics()->QuadsSetSubset without begin");

	m_aTexture[0].u = TlU;	m_aTexture[1].u = BrU;
	m_aTexture[0].v = TlV;	m_aTexture[1].v = TlV;

	m_aTexture[3].u = TlU;	m_aTexture[2].u = BrU;
	m_aTexture[3].v = BrV;	m_aTexture[2].v = BrV;
}

void CGraphics_Threaded::QuadsSetSubsetFree(
	float x0, float y0, float x1, float y1,
	float x2, float y2, float x3, float y3)
{
	m_aTexture[0].u = x0; m_aTexture[0].v = y0;
	m_aTexture[1].u = x1; m_aTexture[1].v = y1;
	m_aTexture[2].u = x2; m_aTexture[2].v = y2;
	m_aTexture[3].u = x3; m_aTexture[3].v = y3;
}

void CGraphics_Threaded::QuadsDraw(CQuadItem *pArray, int Num)
{
	for(int i = 0; i < Num; ++i)
	{
		pArray[i].m_X -= pArray[i].m_Width/2;
		pArray[i].m_Y -= pArray[i].m_Height/2;
	}

	QuadsDrawTL(pArray, Num);
}

void CGraphics_Threaded::QuadsDrawTL(const CQuadItem *pArray, int Num)
{
	CCommandBuffer::SPoint Center;
	Center.z = 0;

	dbg_assert(m_Drawing == DRAWING_QUADS, "called Graphics()->QuadsDrawTL without begin");

	for(int i = 0; i < Num; ++i)
	{
		m_aVertices[m_NumVertices + 4*i].m_Pos.x = pArray[i].m_X;
		m_aVertices[m_NumVertices + 4*i].m_Pos.y = pArray[i].m_Y;
		m_aVertices[m_NumVertices + 4*i].m_Tex = m_aTexture[0];
		m_aVertices[m_NumVertices + 4*i].m_Color = m_aColor[0];

		m_aVertices[m_NumVertices + 4*i + 1].m_Pos.x = pArray[i].m_X + pArray[i].m_Width;
		m_aVertices[m_NumVertices + 4*i + 1].m_Pos.y = pArray[i].m_Y;
		m_aVertices[m_NumVertices + 4*i + 1].m_Tex = m_aTexture[1];
		m_aVertices[m_NumVertices + 4*i + 1].m_Color = m_aColor[1];

		m_aVertices[m_NumVertices + 4*i + 2].m_Pos.x = pArray[i].m_X + pArray[i].m_Width;
		m_aVertices[m_NumVertices + 4*i + 2].m_Pos.y = pArray[i].m_Y + pArray[i].m_Height;
		m_aVertices[m_NumVertices + 4*i + 2].m_Tex = m_aTexture[2];
		m_aVertices[m_NumVertices + 4*i + 2].m_Color = m_aColor[2];

		m_aVertices[m_NumVertices + 4*i + 3].m_Pos.x = pArray[i].m_X;
		m_aVertices[m_NumVertices + 4*i + 3].m_Pos.y = pArray[i].m_Y + pArray[i].m_Height;
		m_aVertices[m_NumVertices + 4*i + 3].m_Tex = m_aTexture[3];
		m_aVertices[m_NumVertices + 4*i + 3].m_Color = m_aColor[3];

		if(m_Rotation != 0)
		{
			Center.x = pArray[i].m_X + pArray[i].m_Width/2;
			Center.y = pArray[i].m_Y + pArray[i].m_Height/2;

			Rotate4(Center, &m_aVertices[m_NumVertices + 4*i]);
		}
	}

	AddVertices(4*Num);
}

void CGraphics_Threaded::QuadsDrawFreeform(const CFreeformItem *pArray, int Num)
{
	dbg_assert(m_Drawing == DRAWING_QUADS, "called Graphics()->QuadsDrawFreeform without begin");

	for(int i = 0; i < Num; ++i)
	{
		m_aVertices[m_NumVertices + 4*i].m_Pos.x = pArray[i].m_X0;
		m_aVertices[m_NumVertices + 4*i].m_Pos.y = pArray[i].m_Y0;
		m_aVertices[m_NumVertices + 4*i].m_Tex = m_aTexture[0];
		m_aVertices[m_NumVertices + 4*i].m_Color = m_aColor[0];

		m_aVertices[m_NumVertices + 4*i + 1].m_Pos.x = pArray[i].m_X1;
		m_aVertices[m_NumVertices + 4*i + 1].m_Pos.y = pArray[i].m_Y1;
		m_aVertices[m_NumVertices + 4*i + 1].m_Tex = m_aTexture[1];
		m_aVertices[m_NumVertices + 4*i + 1].m_Color = m_aColor[1];

		m_aVertices[m_NumVertices + 4*i + 2].m_Pos.x = pArray[i].m_X3;
		m_aVertices[m_NumVertices + 4*i + 2].m_Pos.y = pArray[i].m_Y3;
		m_aVertices[m_NumVertices + 4*i + 2].m_Tex = m_aTexture[3];
		m_aVertices[m_NumVertices + 4*i + 2].m_Color = m_aColor[3];

		m_aVertices[m_NumVertices + 4*i + 3].m_Pos.x = pArray[i].m_X2;
		m_aVertices[m_NumVertices + 4*i + 3].m_Pos.y = pArray[i].m_Y2;
		m_aVertices[m_NumVertices + 4*i + 3].m_Tex = m_aTexture[2];
		m_aVertices[m_NumVertices + 4*i + 3].m_Color = m_aColor[2];
	}

	AddVertices(4*Num);
}

void CGraphics_Threaded::QuadsText(float x, float y, float Size, float r, float g, float b, float a, const char *pText)
{
	float StartX = x;

	QuadsBegin();
	SetColor(r,g,b,a);

	while(*pText)
	{
		char c = *pText;
		pText++;

		if(c == '\n')
		{
			x = StartX;
			y += Size;
		}
		else
		{
			QuadsSetSubset(
				(c%16)/16.0f,
				(c/16)/16.0f,
				(c%16)/16.0f+1.0f/16.0f,
				(c/16)/16.0f+1.0f/16.0f);

			CQuadItem QuadItem(x, y, Size, Size);
			QuadsDrawTL(&QuadItem, 1);
			x += Size/2;
		}
	}

	QuadsEnd();
}

int CGraphics_Threaded::IssueInit()
{
	// issue init command
	m_pCommandBuffer->Reset();
	
	volatile int Result;
	CCommandBuffer::SCommand_Init Cmd;
	str_copy(Cmd.m_aName, "Teeworlds", sizeof(Cmd.m_aName));
	Cmd.m_pResult = &Result;
	Cmd.m_ScreenWidth = g_Config.m_GfxScreenWidth;
	Cmd.m_ScreenHeight = g_Config.m_GfxScreenHeight;
	Cmd.m_FsaaSamples = g_Config.m_GfxFsaaSamples;
	
	Cmd.m_Flags = 0;
	if(g_Config.m_GfxFullscreen) Cmd.m_Flags |= CCommandBuffer::INITFLAG_FULLSCREEN;
	if(g_Config.m_GfxVsync) Cmd.m_Flags |= CCommandBuffer::INITFLAG_VSYNC;
	if(g_Config.m_DbgResizable) Cmd.m_Flags |= CCommandBuffer::INITFLAG_RESIZABLE;

	m_pCommandBuffer->AddCommand(Cmd);

	m_Handler.RunBuffer(m_pCommandBuffer);
	m_Handler.WaitForIdle();
	return Result;
}

int CGraphics_Threaded::InitWindow()
{
	if(IssueInit() == 0)
		return 0;

	// try disabling fsaa
	while(g_Config.m_GfxFsaaSamples)
	{
		g_Config.m_GfxFsaaSamples--;

		if(g_Config.m_GfxFsaaSamples)
			dbg_msg("gfx", "lowering FSAA to %d and trying again", g_Config.m_GfxFsaaSamples);
		else
			dbg_msg("gfx", "disabling FSAA and trying again");

		if(IssueInit() == 0)
			return 0;
	}

	// try lowering the resolution
	if(g_Config.m_GfxScreenWidth != 640 || g_Config.m_GfxScreenHeight != 480)
	{
		dbg_msg("gfx", "setting resolution to 640x480 and trying again");
		g_Config.m_GfxScreenWidth = 640;
		g_Config.m_GfxScreenHeight = 480;

		if(IssueInit() == 0)
			return 0;
	}

	dbg_msg("gfx", "out of ideas. failed to init graphics");

	return -1;
}

bool CGraphics_Threaded::Init()
{
	// fetch pointers
	m_pStorage = Kernel()->RequestInterface<IStorage>();
	m_pConsole = Kernel()->RequestInterface<IConsole>();

	// Set all z to -5.0f
	for(int i = 0; i < MAX_VERTICES; i++)
		m_aVertices[i].m_Pos.z = -5.0f;

	// init textures
	m_FirstFreeTexture = 0;
	for(int i = 0; i < MAX_TEXTURES; i++)
		m_aTextures[i].m_Next = i+1;
	m_aTextures[MAX_TEXTURES-1].m_Next = -1;

	// start the command processor
	m_pProcessor = new CCommandProcessor_SDL_OpenGL;
	m_Handler.Start(m_pProcessor);

	// create command buffers
	m_apCommandBuffers[0] = new CCommandBuffer(1024*512, 1024*1024);
	m_apCommandBuffers[1] = new CCommandBuffer(1024*512, 1024*1024);
	m_pCommandBuffer = m_apCommandBuffers[0];

	// try to init the window
	if(InitWindow() != 0)
		return 0;
	
	// fetch final resolusion
	m_ScreenWidth = g_Config.m_GfxScreenWidth;
	m_ScreenHeight = g_Config.m_GfxScreenHeight;

	// create null texture, will get id=0
	static const unsigned char aNullTextureData[] = {
		0xff,0x00,0x00,0xff, 0xff,0x00,0x00,0xff, 0x00,0xff,0x00,0xff, 0x00,0xff,0x00,0xff,
		0xff,0x00,0x00,0xff, 0xff,0x00,0x00,0xff, 0x00,0xff,0x00,0xff, 0x00,0xff,0x00,0xff,
		0x00,0x00,0xff,0xff, 0x00,0x00,0xff,0xff, 0xff,0xff,0x00,0xff, 0xff,0xff,0x00,0xff,
		0x00,0x00,0xff,0xff, 0x00,0x00,0xff,0xff, 0xff,0xff,0x00,0xff, 0xff,0xff,0x00,0xff,
	};

	m_InvalidTexture = LoadTextureRaw(4,4,CImageInfo::FORMAT_RGBA,aNullTextureData,CImageInfo::FORMAT_RGBA,TEXLOAD_NORESAMPLE);
	return 0;
}

void CGraphics_Threaded::Shutdown()
{
	// add swap command
	CCommandBuffer::SCommand_Shutdown Cmd;
	m_pCommandBuffer->AddCommand(Cmd);
	m_Handler.RunBuffer(m_pCommandBuffer);
	
	// wait for everything to process and then stop the command processor
	m_Handler.WaitForIdle();
	m_Handler.Stop();
	delete m_pProcessor;
	m_pProcessor = 0;
}

void CGraphics_Threaded::Minimize()
{
	SDL_WM_IconifyWindow();
}

void CGraphics_Threaded::Maximize()
{
	// TODO: SDL
}

int CGraphics_Threaded::WindowActive()
{
	return SDL_GetAppState()&SDL_APPINPUTFOCUS;
}

int CGraphics_Threaded::WindowOpen()
{
	return SDL_GetAppState()&SDL_APPACTIVE;

}

void CGraphics_Threaded::TakeScreenshot(const char *pFilename)
{
	// TODO: screenshot support
	char aDate[20];
	str_timestamp(aDate, sizeof(aDate));
	str_format(m_aScreenshotName, sizeof(m_aScreenshotName), "screenshots/%s_%s.png", pFilename?pFilename:"screenshot", aDate);
	m_DoScreenshot = true;
}

void CGraphics_Threaded::Swap()
{
	// TODO: screenshot support
	if(m_DoScreenshot)
	{
		ScreenshotDirect(m_aScreenshotName);
		m_DoScreenshot = false;
	}

	// add swap command
	CCommandBuffer::SCommand_Swap Cmd;
	m_pCommandBuffer->AddCommand(Cmd);

	// kick the command buffer
	KickCommandBuffer();
}

// syncronization
void CGraphics_Threaded::InsertSignal(semaphore *pSemaphore)
{
	CCommandBuffer::SCommand_Signal Cmd;
	Cmd.m_pSemaphore = pSemaphore;
	m_pCommandBuffer->AddCommand(Cmd);
}

bool CGraphics_Threaded::IsIdle()
{
	return m_Handler.IsIdle();
}

void CGraphics_Threaded::WaitForIdle()
{
	m_Handler.WaitForIdle();
}

int CGraphics_Threaded::GetVideoModes(CVideoMode *pModes, int MaxModes)
{
	// TODO: fix support for video modes, using fake modes for now
	//int NumModes = sizeof(g_aFakeModes)/sizeof(CVideoMode);
	//SDL_Rect **ppModes;

	//if(g_Config.m_GfxDisplayAllModes)
	{
		int Count = sizeof(g_aFakeModes)/sizeof(CVideoMode);
		mem_copy(pModes, g_aFakeModes, sizeof(g_aFakeModes));
		if(MaxModes < Count)
			Count = MaxModes;
		return Count;
	}

	// TODO: fix this code on osx or windows
	/*

	ppModes = SDL_ListModes(NULL, SDL_OPENGL|SDL_GL_DOUBLEBUFFER|SDL_FULLSCREEN);
	if(ppModes == NULL)
	{
		// no modes
		NumModes = 0;
	}
	else if(ppModes == (SDL_Rect**)-1)
	{
		// all modes
	}
	else
	{
		NumModes = 0;
		for(int i = 0; ppModes[i]; ++i)
		{
			if(NumModes == MaxModes)
				break;
			pModes[NumModes].m_Width = ppModes[i]->w;
			pModes[NumModes].m_Height = ppModes[i]->h;
			pModes[NumModes].m_Red = 8;
			pModes[NumModes].m_Green = 8;
			pModes[NumModes].m_Blue = 8;
			NumModes++;
		}
	}

	return NumModes;*/
}


extern IEngineGraphics *CreateEngineGraphicsThreaded() { return new CGraphics_Threaded(); }