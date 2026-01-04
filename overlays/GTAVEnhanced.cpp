#include "GTAVEnhanced.h"
#include "macros.h"

using namespace OF;

void GTAVEnhanced::Setup() {
	InitFramework(device, spriteBatch, window);
	initMacros();
}

void GTAVEnhanced::Render() {
	InputHandler::executeFirstQueuedTask();
}