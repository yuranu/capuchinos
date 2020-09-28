#include "nc_lyt.hpp"
#include <algorithm>
#include <cassert>

nc_lyt::nc_lyt(nc_lyt *parent) : parent(parent) {
    if (parent)
        parent->add(this);
}

void nc_lyt::get_dim(nc_lyt *asker, int &h, int &w, int &y, int &x) {
    assert(false);
    (void)asker;
    (void)h;
    (void)w;
    (void)y;
    (void)x;
}

bool nc_lyt::place_cursor() {
    bool rv = false;
    iter_predicate func = [&rv](nc_lyt *lyt) -> bool {
        if (lyt->active)
            rv |= lyt->place_cursor();
        if (rv)
            return true;
        return false;
    };
    this->visit(func);
    return rv;
}

void nc_lyt::redraw() {
    iter_predicate func = [](nc_lyt *lyt) -> bool {
        if (lyt->active)
            lyt->redraw();
        return false;
    };
    this->visit(func);
}

void nc_lyt::refresh() {
    iter_predicate func = [](nc_lyt *lyt) -> bool {
        if (lyt->active)
            lyt->refresh();
        return false;
    };
    this->visit(func);
}

bool nc_lyt::process_input(int ch) {
    iter_predicate func = [ch](nc_lyt *lyt) -> bool {
        if (lyt->active)
            return lyt->process_input(ch);
        return false;
    };
    return this->visit(func);
}

std::vector<nc_lyt *> nc_lyt::vec_all() {
    std::vector<nc_lyt *> vec;
    iter_predicate func = [&vec, &func](nc_lyt *lyt) -> bool {
        if (lyt->active) {
            vec.push_back(lyt);
            return lyt->visit(func);
        }
        return false;
    };
    this->visit(func);
    return vec;
}

void nc_lyt::move_focus(int dir) {
    assert(dir == 1 || dir == -1);
    auto vec = this->vec_all();
    int focused;
    for (focused = 0; focused < (int)vec.size(); ++focused) {
        if (vec[focused]->focused)
            break;
    }
    if (focused < (int)vec.size())
        vec[focused]->focused = false;
    for (int i = 0; i < (int)vec.size(); ++i) {
        focused += dir;
        if (focused < 0)
            focused = (int)vec.size() - 1;
        if (focused >= (int)vec.size())
            focused = 0;
        if (vec[focused]->focusable) {
            vec[focused]->focused = true;
            break;
        }
    }
}

void nc_lyt::set_focus_to(nc_lyt *dst) {
    auto vec = this->vec_all();
    int focused;
    for (focused = 0; focused < (int)vec.size(); ++focused) {
        if (vec[focused]->focused)
            break;
    }
    if (focused < (int)vec.size())
        vec[focused]->focused = false;
    dst->focused = true;
}

void nc_lyt_pln::add(nc_lyt *subl) {
    assert(!this->subl);
    this->subl = subl;
}

void nc_lyt_pln::remove(nc_lyt *subl) {
    assert(this->subl == subl);
    subl->detach();
    this->subl = NULL;
}

void nc_lyt_pln::get_dim(nc_lyt *asker, int &h, int &w, int &y, int &x) {
    assert(this->parent);
    assert(asker == this->subl);
    this->parent->get_dim(this, h, w, y, x);
}

bool nc_lyt_pln::visit(iter_predicate &act) {
    if (this->subl) {
        return act(this->subl);
    }
    return false;
}

void nc_lyt_flow::add(nc_lyt *subl) { this->subls.push_back(subl); }

void nc_lyt_flow::remove(nc_lyt *subl) {
    assert(std::find(this->subls.begin(), this->subls.end(), subl) !=
           this->subls.end());
    subl->detach();
    this->subls.remove(subl);
}

void nc_lyt_flow::get_dim(nc_lyt *asker, int &h, int &w, int &y, int &x) {
    std::list<nc_lyt *> active_subls;
    for (auto subl : this->subls)
        if (subl->active)
            active_subls.push_back(subl);
    auto len = active_subls.size();
    assert(len);
    int _h, _w, _y, _x;
    this->parent->get_dim(this, _h, _w, _y, _x);
    y = _y, x = _x;
    if (this->horizontal) {
        int total_extra = 0;
        int want_extra = 0;
        int d = _w / len;
        h = _h, w = d;
        for (auto subl : active_subls) {
            if (subl->max_w < d) {
                total_extra += d - subl->max_w;
            } else {
                want_extra++;
            }
        }
        int extra = want_extra ? total_extra / want_extra : 0;
        for (auto subl : active_subls) {
            if (subl->max_w < d)
                w = subl->max_w;
            else if (!--len)
                w = _w - x;
            else
                w = d + extra;
            if (subl == asker)
                return;
            x += w;
        }
    } else {
        int total_extra = 0;
        int want_extra = 0;
        int d = _h / len;
        h = d, w = _w;
        for (auto subl : active_subls) {
            if (subl->max_h < d) {
                total_extra += d - subl->max_h;
            } else {
                want_extra++;
            }
        }
        int extra = want_extra ? total_extra / want_extra : 0;
        for (auto subl : active_subls) {
            if (subl->max_h < d)
                h = subl->max_h;
            else if (!--len)
                h = _h - y;
            else
                h = d + extra;
            if (subl == asker)
                return;
            y += h;
        }
    }
    assert(false);
}

bool nc_lyt_flow::visit(iter_predicate &act) {
    for (auto subl : this->subls) {
        if (act(subl))
            return true;
    }
    return false;
}
