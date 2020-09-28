#include "ncctx.hpp"
#include <cassert>
#include <signal.h>
#include <sstream>

#include <ncurses.h>

ncctx *ncctx::singleton = NULL;
void (*ncctx::old_sigwinch_hndlr)(int sig) = NULL;

void ncctx::sigwinch_hndlr(int sig) {
    assert(sig == SIGWINCH);
    assert(ncctx::singleton);
    ncctx::singleton->redraw();
    ncctx::singleton->refresh();
    if (ncctx::old_sigwinch_hndlr)
        ncctx::old_sigwinch_hndlr(sig);
}

ncctx::ncctx() : nc_lyt_pln(NULL) {
    assert(!ncctx::singleton);
    assert(!ncctx::old_sigwinch_hndlr);

    initscr();
    noecho();
    cbreak();

    curs_set(this->is_cursor);

    ncctx::singleton = this;
    ncctx::old_sigwinch_hndlr = signal(SIGWINCH, ncctx::sigwinch_hndlr);
}

ncctx::~ncctx() {
    signal(SIGWINCH, ncctx::old_sigwinch_hndlr);
    ncctx::singleton = NULL;
    endwin();
}

void ncctx::refresh() {
    wrefresh(stdscr);
    nc_lyt_pln::refresh();
    if (nc_lyt_pln::place_cursor() != this->is_cursor) {
        this->is_cursor = !this->is_cursor;
        curs_set(this->is_cursor);
    }
}

void ncctx::get_dim(nc_lyt *asker, int &h, int &w, int &y, int &x) {
    (void)asker;
    getmaxyx(stdscr, h, w);
    x = y = 0;
}

bool ncctx::process_input(int ch) {
    if (ch == '\t') {
        this->move_focus(1);
        return true;
    }
    return nc_lyt_pln::process_input(ch);
}

nc_win::nc_win(nc_lyt *parent, const std::string name, bool border)
    : nc_lyt(parent) {
    this->focusable = true;
    this->name = name;
    this->parent->get_dim(this, this->h, this->w, this->y, this->x);
    if (border) {
        assert(brdwin = newwin(h, w, y, x));
        assert(win = derwin(brdwin, h - 2, w - 2, 1, 1));
    } else {
        brdwin = NULL;
        assert(win = newwin(h, w, y, x));
    }
}

nc_win::~nc_win() {
    delwin(this->win);
    if (this->brdwin)
        delwin(this->brdwin);
}

void nc_win::redraw() {
    int h, w, y, x;
    this->parent->get_dim(this, h, w, y, x);
    if (h != this->h || w != this->w || x != this->x || y != this->y) {
        if (this->brdwin) {
            wresize(this->brdwin, h, w);
            mvwin(this->brdwin, y, x);
            wresize(this->win, h - 2, w - 2);
            mvwin(this->win, 1, 1);
        } else {
            wresize(this->win, h, w);
            mvwin(this->win, y, x);
        }
        this->h = h, this->w = w, this->x = x, this->y = y;
    }

    if (this->brdwin) {
        werase(this->brdwin);
        if (this->focused)
            /*wborder(this->brdwin, '|', '|', '-', '-', '+', '+', '+', '+');*/
            /*wborder(this->brdwin, 'H', 'H', '=', '=', '/', '\\', '\\', '/');*/
            wborder(this->brdwin, 0, 0, '=', '=', 0, 0, 0, 0);
        else
            box(this->brdwin, 0, 0);
        mvwprintw(this->brdwin, 0, 1, ("<" + this->name + ">").c_str());
    }
    werase(this->win);
    this->on_draw();
    for (auto delegate : this->on_draw_listeners) {
        delegate(this);
    }
}

void nc_win::refresh() {
    if (this->brdwin)
        wrefresh(this->brdwin);
    else
        wrefresh(this->win);
}

void nc_win_inp::on_draw() {
    std::string line = this->greet + this->line;
    const int w = this->get_w();
    const int len = line.length();
    if (len >= w) {
        const int cursor = this->cursor + this->greet.length();
        if (cursor < w) {
            line = line.substr(0, w - 1);
        } else {
            line = line.substr(cursor - w + 1, w - 1);
        }
    }
    werase(this->win);
    mvwprintw(this->win, 0, 0, (line).c_str());
}

nc_win_inp::nc_win_inp(nc_lyt *parent, const std::string name,
                       const std::string greet, bool border)
    : nc_win(parent, name, border), greet(greet) {
    wtimeout(stdscr, 500);
    keypad(stdscr, true);
}

bool nc_win_inp::process_input(int ch) {
    if (!this->focused)
        return false;
    switch (ch) {
    case KEY_ENTER:
    case '\n':
        if (this->on_input())
            break;
        for (auto delegate : this->on_input_listeners) {
            if (delegate(this, this->line))
                break;
        }
        if (this->hist.empty() ||
            this->hist[this->hist.size() - 1] != this->line) {
            this->hist.push_back(line);
            this->hist_ptr = this->hist.size();
        }
        this->line = "";
        this->cursor = 0;
        break;
    case KEY_BACKSPACE:
    case 127:
        if (this->cursor > 0) {
            this->line.erase(this->cursor - 1, 1);
            this->cursor--;
        }
        break;
    case KEY_DC:
        if (this->cursor < (int)this->line.length()) {
            this->line.erase(this->cursor, 1);
        }
        break;
    case KEY_LEFT:
        if (this->cursor > 0) {
            this->cursor--;
        }
        break;
    case KEY_RIGHT:
        if (this->cursor < (int)this->line.length()) {
            this->cursor++;
        }
        break;
    case KEY_UP:
        if (!this->hist.empty()) {
            if (this->hist_ptr > 0)
                this->hist_ptr--;
            this->line = this->hist[this->hist_ptr];
            this->cursor = this->line.length();
        }
        break;
    case KEY_DOWN:
        if (!this->hist.empty()) {
            if (this->hist_ptr < (int)this->hist.size() - 1) {
                this->hist_ptr++;
                this->line = this->hist[this->hist_ptr];
                this->cursor = this->line.length();
            } else if (this->hist_ptr == (int)this->hist.size() - 1) {
                this->hist_ptr++;
                this->line = "";
                this->cursor = 0;
            }
        }
        break;
    default:
        if (ch >= 0 && isprint(ch)) {
            this->line.insert(cursor, 1, ch);
            this->cursor++;
        }
        return false;
    }
    return true;
}

bool nc_win_inp::place_cursor() {
    const int cursor = this->cursor + (int)this->greet.length() >= this->get_w()
                           ? this->get_w() - 1
                           : this->cursor + this->greet.length();
    move(this->get_y(), this->get_x() + cursor);
    return true;
}

void nc_win_txt::on_draw() {
    if (this->viewport < 0) {
        this->viewport = 0;
    }

    std::stringstream ss(this->lines);
    std::string line;
    int i = 0;

    while (std::getline(ss, line, '\n')) {
        if (i >= this->viewport) {
            mvwprintw(this->get_win(), i - this->viewport, 0, line.c_str());
        }
        i++;
    }

    if (i && i - 1 < this->viewport) {
        this->viewport = i; /* Don't let viewport run away */
    }
}

nc_win_txt::nc_win_txt(nc_lyt *parent, const std::string name, bool border)
    : nc_win(parent, name, border) {}

bool nc_win_txt::process_input(int ch) {
    if (!this->focused)
        return false;
    switch (ch) {
    case KEY_UP:
        this->viewport--;
        return true;
    case KEY_DOWN:
        this->viewport++;
        return true;
    }
    return false;
}