﻿#pragma once

#include <Uis/UiObject.hpp>
#include <Maths/Timer.hpp>
#include <Fonts/Text.hpp>

using namespace acid;

namespace test
{
	class OverlayDebug :
		public UiObject
	{
	public:
		explicit OverlayDebug(UiObject *parent);

		void UpdateObject() override;
	private:
		std::unique_ptr<Text> CreateStatus(const std::string &content, const float &positionX, const float &positionY, const Text::Justify &justify);

		std::unique_ptr<Text> m_textFps;
		std::unique_ptr<Text> m_textUps;
		Timer m_timerUpdate;
	};
}
