/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "AppTaskZephyr.h"

class AppTask: public chip::Zephyr::App::AppTaskZephyr
{
      public:
	~AppTask() override {};
	void PreInitMatterStack(void) override;
	void PostInitMatterServerInstance(void) override;

	static AppTask &GetDefaultInstance();

      private:
	static AppTask sAppTask;
};
