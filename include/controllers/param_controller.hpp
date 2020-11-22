// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#ifndef __PARAM_CONTROLLER_HPP__
#define __PARAM_CONTROLLER_HPP__

#include "global.hpp"
#include "controllers/event_mgr.hpp"

class ParamController
{
  private:
    static constexpr char const * TAG = "ParamController";

    bool form_is_shown;
    bool wait_for_key_after_wifi;

  public:
    ParamController() : form_is_shown(false), wait_for_key_after_wifi(false) { };
    void key_event(EventMgr::KeyEvent key);
    void enter();
    void leave(bool going_to_deep_sleep = false);

    inline void set_form_is_shown() { form_is_shown = true; }
    inline void set_wait_for_key_after_wifi() { wait_for_key_after_wifi = true; form_is_shown = false; }
};

#if __PARAM_CONTROLLER__
  ParamController param_controller;
#else
  extern ParamController param_controller;
#endif

#endif