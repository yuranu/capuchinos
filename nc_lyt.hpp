#pragma once

#include <ncurses.h>

#include <cassert>
#include <climits>
#include <list>

#include <functional>

class nc_lyt;
class nc_lyt_pln;
class nc_lyt_flow;

class nc_lyt {
  protected: /* members */
    nc_lyt *parent;
    bool focusable = false;
    bool focused = false;

  public: /* properties */
    /* Dimentions are not respected by all layouts */
    int max_h = INT_MAX;
    int max_w = INT_MAX;
    bool active = true;

  public:
    typedef std::function<bool(nc_lyt *)> iter_predicate;

  public: /* default interface */
    nc_lyt(nc_lyt *parent);
    void detach() { this->parent = NULL; }

    virtual bool visit(iter_predicate &act) { return false; }

    virtual void add(nc_lyt *subl) { assert(false); }
    virtual void remove(nc_lyt *subl) { assert(false); }
    virtual void redraw();
    virtual void refresh();
    virtual bool place_cursor();
    virtual void get_dim(nc_lyt *asker, int &h, int &w, int &y, int &x);
    virtual bool process_input(int ch);
  public: /* Non overridable helpers */
    std::vector<nc_lyt*> vec_all();
    void move_focus(int dir);
    void set_focus_to(nc_lyt *);
};

class nc_lyt_pln : public nc_lyt {
  protected: /* members */
    nc_lyt *subl = NULL;

  public: /* interface */
    nc_lyt_pln(nc_lyt *parent) : nc_lyt(parent) {}
    virtual void add(nc_lyt *subl) override;
    virtual void remove(nc_lyt *subl) override;

  public: /* overrides */
    virtual void get_dim(nc_lyt *asker, int &h, int &w, int &y,
                         int &x) override;
    virtual bool visit(iter_predicate &act) override;
};

class nc_lyt_flow : public nc_lyt {
  protected: /* members */
    std::list<nc_lyt *> subls;
    bool horizontal;

  public: /* interface */
    nc_lyt_flow(nc_lyt *parent, bool horizontal = true)
        : nc_lyt(parent), horizontal(horizontal) {}
    virtual void add(nc_lyt *subl) override;
    virtual void remove(nc_lyt *subl) override;

  public: /* overrides */
    virtual void get_dim(nc_lyt *asker, int &h, int &w, int &y,
                         int &x) override;
    virtual bool visit(iter_predicate &act) override;
};
