#pragma once

#include <atomic>
#include <ncurses.h>
#include <string>

#include <cassert>
#include <functional>
#include <list>
#include <vector>

#include "nc_lyt.hpp"

class ncctx;
class nc_win;

class ncctx : public nc_lyt_pln {
  private:
    static void sigwinch_hndlr(int sig);
    static void (*old_sigwinch_hndlr)(int sig);
    static ncctx *singleton;

  protected:
    bool is_cursor = false;

  public:
    ncctx();
    virtual ~ncctx();

    virtual void refresh() override;
    virtual void get_dim(nc_lyt *asker, int &h, int &w, int &y,
                         int &x) override;
    virtual bool process_input(int ch) override;
};

class nc_win : public nc_lyt {
  protected:
    WINDOW *win, *brdwin;
    int h, w, x, y;

  public: /* Properties */
    std::string name;
    int get_h() { return brdwin ? h - 2 : h; }
    int get_w() { return brdwin ? w - 2 : w; }
    int get_y() { return brdwin ? y + 1 : y; }
    int get_x() { return brdwin ? x + 1 : x; }

  public: /* delegates */
    /* I know it is not C++ way, but I like the idea. Lets give it a try. */
    std::list<std::function<bool(nc_win *)>> on_draw_listeners;

  public:
    nc_win(nc_lyt *parent, const std::string name = "", bool border = true);
    WINDOW *get_win() { return this->win; }
    virtual ~nc_win();
    virtual void redraw() override;
    virtual void refresh() override;
    virtual bool process_input(int ch) override { return false; }
    virtual bool place_cursor() override { return false; }
    virtual void on_draw() {}
};

class nc_win_inp : public nc_win {
  protected:
    std::string greet = "";

    std::string last_line = "";
    bool line_read = false;
    std::string line = "";
    std::vector<std::string> hist;
    int hist_ptr;

    int cursor = 0;

  public: /* delegates */
    std::list<std::function<bool(nc_win *, std::string const &)>>
        on_input_listeners;

  public:
    nc_win_inp(nc_lyt *parent, const std::string name = "",
               const std::string greet = "> ", bool border = true);
    virtual bool process_input(int ch) override;
    virtual bool place_cursor() override;
    virtual void on_draw() override;
    virtual bool on_input() { return false; }
};

class nc_win_txt : public nc_win {
  protected:
    int viewport = 0;

  public:
    std::string lines;

  public:
    nc_win_txt(nc_lyt *parent, const std::string name, bool border = true);
    virtual bool process_input(int ch) override;
    virtual void on_draw() override;
};