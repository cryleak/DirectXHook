#pragma once

#include "IRenderCallback.h"
#include "OverlayFramework.h"

class GTAVEnhanced : public IRenderCallback
{
public:
	void Setup();
	void Render();
};