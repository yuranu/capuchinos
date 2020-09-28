#include "ncctx.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

class disk_sim {
    friend class view;

  public:
    struct disk_conf {
        long consume_per_second = 32;
    } & conf;

  private:
    std::atomic<std::chrono::steady_clock::time_point> expected_finish;

  public:
    disk_sim(disk_conf &conf)
        : conf(conf), expected_finish(std::chrono::steady_clock::now()) {}

    std::chrono::steady_clock::time_point add_jobs(int count) {
        auto excpected_duration = std::chrono::nanoseconds(
            1000000000UL * count / this->conf.consume_per_second);
        while (1) {
            auto prev_expected_finish = this->expected_finish.load();
            auto new_expected_finish =
                (prev_expected_finish < std::chrono::steady_clock::now()
                     ? std::chrono::steady_clock::now()
                     : prev_expected_finish) +
                excpected_duration;
            if (this->expected_finish.compare_exchange_strong(
                    prev_expected_finish, new_expected_finish)) {
                return new_expected_finish;
            }
        }
    }
};

struct resource {
    int id;
    int batch_id;
};

class pool {
    friend class view;

  public:
    struct pool_conf {
        long total_rsc = 1000;
        long flush_size = 8;
        long flush_timeout_ns = 2000000000UL;
        long min_greed = 1;
        long max_greed = 20;
        long min_bufs = 2;
        long reserve = 100;
    } & conf;

    struct {
        std::atomic_int locks_taken = 0;
        std::atomic_int bufs_lost = 0;
    } stats;

  public:
    struct {
        std::mutex guard;
        std::list<resource> free;
        unsigned long total_pressure = 0;
    } run;

    pool(pool_conf &conf) : conf(conf) {
        std::unique_lock<std::mutex> lk(this->run.guard);
        for (int i = 0; i < this->conf.total_rsc; ++i) {
            this->run.free.push_back({.id = i});
        }
    }
};

class capuch {
    friend class view;
    friend class simulation;

  private: /* Internal */
    int id;
    pool &p;
    disk_sim &disk;
    std::list<resource> free_list;
    std::list<resource> ready_list;
    std::optional<resource> active_rsc;
    int batch_size = 0;
    int batch_id = 0;
    int greed = 0;

    struct {
        std::chrono::steady_clock::time_point last_ready;
        std::chrono::steady_clock::time_point flush_start;
        std::chrono::steady_clock::time_point flush_finish;
        bool flush_ready;
        bool flushing;
    } thread_state;

  public: /* Properties */
    int priority = 10;
    struct {
        bool running = true;
        int ready_per_sec = 1;
    } simulation;

    struct {
        int greed_inc = 0;
        int greed_dec = 0;
        int timeout = 0;
    } stats;

  private: /* Internal methods */
    void set_priority(int priority) {
        if (this->greed < this->p.conf.max_greed) {
            std::unique_lock<std::mutex> lk(this->p.run.guard);
            if (this->greed)
                this->p.run.total_pressure -= this->pressure();
            this->priority = priority;
            this->p.run.total_pressure += this->pressure();
        }
    }
    void inc_greed() {
        if (this->greed < this->p.conf.max_greed) {
            this->stats.greed_inc++;
            std::unique_lock<std::mutex> lk(this->p.run.guard);
            this->p.stats.locks_taken++;
            if (this->greed)
                this->p.run.total_pressure -= this->pressure();
            ++this->greed;
            if (this->greed < this->p.conf.min_greed)
                this->greed = this->p.conf.min_greed;
            this->p.run.total_pressure += this->pressure();
        }
    }
    void dec_greed() {
        if (this->greed > this->p.conf.min_greed) {
            this->stats.greed_dec++;
            std::unique_lock<std::mutex> lk(this->p.run.guard);
            this->p.stats.locks_taken++;
            if (this->greed)
                this->p.run.total_pressure -= this->pressure();
            --this->greed;
            if (this->greed > this->p.conf.max_greed)
                this->greed = this->p.conf.max_greed;
            this->p.run.total_pressure += this->pressure();
        }
    }

    void sync_quota() {
        if (this->quota() < this->nbufs()) {
            /* Return buffers to the pool */
            std::unique_lock<std::mutex> lk(this->p.run.guard);
            this->p.stats.locks_taken++;
            do {
                if (!this->free_list.empty()) {
                    this->p.run.free.push_back(this->free_list.front());
                    this->free_list.pop_front();
                } else if (!this->ready_list.empty()) {
                    this->p.run.free.push_back(this->ready_list.front());
                    this->ready_list.pop_front();
                } else
                    assert(false); /* We have 0 nbufs, so what, quota < 0? */
            } while (this->quota() < this->nbufs());
        } else if (this->quota() > this->nbufs()) {
            /* Get buffers to the pool */
            std::unique_lock<std::mutex> lk(this->p.run.guard);
            this->p.stats.locks_taken++;
            do {
                if (this->p.run.free.empty())
                    break;
                this->free_list.push_back(this->p.run.free.front());
                this->p.run.free.pop_front();
            } while (this->quota() > this->nbufs());
        } else
            assert(false); /* Not supposed to call this if nbufs == quota */
    }

  public: /* Calculated properties */
    int nbufs() {
        return this->free_list.size() + this->ready_list.size() +
               this->active_rsc.has_value();
    }
    int quota() {
        auto tp = this->p.run.total_pressure;
        if (!tp)
            return 0;
        return std::max((unsigned long)this->p.conf.min_bufs,
                        this->pressure() *
                            (this->p.conf.total_rsc - this->p.conf.reserve) /
                            tp);
    }
    unsigned long pressure() {
        return (unsigned long)(1 << this->greed) * this->priority;
    }

  public:
    capuch(int id, pool &p, disk_sim &disk) : id(id), p(p), disk(disk) {}

  private: /* Events */
    void on_ready() {
        if (this->active_rsc.has_value()) {
            this->active_rsc->batch_id = this->batch_id;
            this->ready_list.push_back(*this->active_rsc);
            ++this->batch_size;
            this->active_rsc.reset();
        }

        /* Do we need to trigger ready event? */
        if (this->batch_size >= this->p.conf.flush_size) {
            this->thread_state.flush_ready = true;
        }

        bool had_to_inc_greed = false;
        if (!this->free_list.empty()) {
            /* Enought resorces in the free list */
            this->active_rsc = this->free_list.front();
            this->free_list.pop_front();
        } else {
            this->inc_greed();
            had_to_inc_greed = true;
        }

        if (this->nbufs() != this->quota())
            this->sync_quota();

        if (had_to_inc_greed) {
            if (!this->free_list.empty()) {
                /* Enought resorces in the free list */
                this->active_rsc = this->free_list.front();
                this->free_list.pop_front();
            } else {
                assert(this->ready_list.size());
                this->active_rsc = this->ready_list.front();
                this->ready_list.pop_front();

                /* Lost data */
                this->p.stats.bufs_lost++;
                this->thread_state.flush_ready = true; /* Flush. Urgent. */
            }
        }

        assert(this->active_rsc.has_value());
    }

    void on_flush_start() {

        assert(!this->thread_state.flushing);
        assert(this->thread_state.flush_ready);
        assert(this->batch_size);

        this->batch_id++;
        this->thread_state.flush_ready = false;
        this->thread_state.flushing = true;
        this->thread_state.flush_start = std::chrono::steady_clock::now();
        this->thread_state.flush_finish = this->disk.add_jobs(this->batch_size);
        this->batch_size = 0;
    }

    void on_flush_finish() {

        assert(std::chrono::steady_clock::now() >=
               this->thread_state.flush_finish);
        assert(this->thread_state.flushing);

        auto expected_batch_id = this->batch_id - 1;
        auto i = this->ready_list.begin();
        while (i != this->ready_list.end()) {
            if (i->batch_id == expected_batch_id) {
                auto r = *i;
                this->ready_list.erase(i++);
                this->free_list.push_back(r);
            } else
                break;
        }

        this->thread_state.flushing = false;
    }

    void on_timeout() {
        assert(!this->thread_state.flushing);
        assert(!this->thread_state.flush_ready);

        this->stats.timeout++;

        if (this->free_list.size() > this->ready_list.size())
            this->dec_greed();

        if (this->nbufs() > this->quota())
            this->sync_quota();
        /* Important: case nbufs < quota is not handled on timeout. I means that
         * quota gives us more resources than we have. However, since it is
         * timeout, it means we alredy have enough resorces. Hence prefer not to
         * hog on resources, even though quota allows. */

        if (this->batch_size) {
            this->thread_state.flush_ready = true;
            this->on_flush_start();
        }
    }

  public: /* Main thread loop */
    void main() {
        this->thread_state.last_ready = std::chrono::steady_clock::now();
        this->thread_state.flush_start = std::chrono::steady_clock::now();
        this->thread_state.flush_finish = std::chrono::steady_clock::now();
        this->thread_state.flush_ready = false;
        this->thread_state.flushing = false;

        while (this->simulation.running) {
            auto now = std::chrono::steady_clock::now();

            /* First check timeout case - we are not flushing and not ready
             * and last flush finished more then X seconds ago*/
            if (!this->thread_state.flushing &&
                !this->thread_state.flush_ready &&
                now > this->thread_state.flush_finish &&
                (now - this->thread_state.flush_finish) >=
                    std::chrono::nanoseconds(this->p.conf.flush_timeout_ns)) {
                this->on_timeout();
            }

            /* Calculate how many new buffers were created since last
             * iteration. Invoke ready event for each. */
            auto n_new_ready = std::chrono::duration_cast<std::chrono::seconds>(
                                   now - this->thread_state.last_ready)
                                   .count() *
                               this->simulation.ready_per_sec;
            for (int i = 0; i < n_new_ready; ++i) {
                this->on_ready();
                this->thread_state.last_ready = now;
            }

            /* If we are currently flushing and flush finish time has passed
             * - it is time to trigger flush finish event. */
            if (this->thread_state.flushing &&
                now >= this->thread_state.flush_finish) {
                this->on_flush_finish();
            }

            /* If we re not flushing (NOT else-if, both can be correct in
             * THIS order, important) and we have more ready - it is flush
             * start event. */
            if (!this->thread_state.flushing &&
                this->thread_state.flush_ready) {
                assert(now >= this->thread_state.flush_finish);
                this->on_flush_start();
            }

            /* @TODO: smart sleep, calculate next event time */
            std::this_thread::sleep_for(std::chrono::nanoseconds(100000000));
        }
    }
};

class simulation {
    friend class view;

  public:
    struct {
        long ncapuch = 12;
    } conf;
    pool::pool_conf pool_conf;
    disk_sim::disk_conf disk_conf;

    std::map<std::string, long &> conf_map = {
        {"conf.ncapuch", conf.ncapuch},

        {"pool_conf.flush_size", pool_conf.flush_size},
        {"pool_conf.flush_timeout_ns", pool_conf.flush_timeout_ns},
        {"pool_conf.min_greed", pool_conf.min_greed},
        {"pool_conf.max_greed", pool_conf.max_greed},
        {"pool_conf.min_bufs", pool_conf.min_bufs},
        {"pool_conf.reserve", pool_conf.reserve},
        {"pool_conf.total_rsc", pool_conf.total_rsc},

        {"disk_conf.consume_per_second", disk_conf.consume_per_second},
    };

  private:
    bool running = false;
    std::vector<std::thread> capuches_threads;
    std::vector<capuch> capuches;
    pool *p;
    disk_sim *disk;

  public:
    simulation(bool start = false) {
        if (start) {
            this->start();
        }
    };
    ~simulation() { this->terminate(); }
    bool is_running() { return this->running; }
    const std::vector<capuch> &get_capuches() { return this->capuches; }
    void start() {
        assert(!this->running);
        this->running = true;
        this->p = new pool(this->pool_conf);
        this->disk = new disk_sim(this->disk_conf);
        this->capuches.reserve(this->conf.ncapuch);
        this->capuches_threads.reserve(this->conf.ncapuch);

        /* Initialization is in three phases */

        /* 1. Create and set initial greed */
        for (int i = 0; i < this->conf.ncapuch; ++i) {
            this->capuches.emplace_back(i, *this->p, *this->disk);
            this->capuches[i].inc_greed();
        }

        /* 2. Get first buffers */
        for (int i = 0; i < this->conf.ncapuch; ++i) {
            this->capuches[i].sync_quota();
        }

        /* 3. After the first 2 synchronously done, start async workers */
        for (int i = 0; i < this->conf.ncapuch; ++i) {
            this->capuches_threads.emplace_back(&capuch::main,
                                                &this->capuches[i]);
        }
    }
    void terminate() {
        if (this->running) {
            for (auto &capuch : this->capuches)
                capuch.simulation.running = false;
            for (auto &t : this->capuches_threads)
                t.join();
            this->capuches.clear();
            this->capuches_threads.clear();
            delete this->p;
            delete this->disk;
            this->running = false;
        }
    }
};

class view {
  private:
    simulation &sim;
    bool running = true;

  private:
    bool command_dispatcher(const std::string &_cmd) {
        std::stringstream ss(_cmd);
        std::string cmd;
        ss >> cmd;
        if (cmd == "quit") {
            this->sim.terminate();
            this->running = false;
        } else if (cmd == "start") {
            if (!this->sim.is_running())
                this->sim.start();
        } else if (cmd == "term") {
            this->sim.terminate();
        } else if (this->sim.is_running() && cmd.rfind("capuch", 0) == 0) {
            int start, end;
            ss >> start >> end;
            if (start < 0)
                start = 0;
            if (end >= this->sim.conf.ncapuch)
                end = this->sim.conf.ncapuch - 1;
            if (start <= end) {
                std::string subcmd;
                int value;
                ss >> subcmd >> value;
                if (subcmd == "speed") {
                    for (int i = start; i <= end; ++i)
                        this->sim.capuches[i].simulation.ready_per_sec = value;
                } else if (subcmd == "priority") {
                    for (int i = start; i <= end; ++i) {
                        this->sim.capuches[i].set_priority(value);
                    }
                }
            }
        } else if (this->sim.is_running() && cmd.rfind("disk-flush", 0) == 0) {
            std::string subcmd;
            this->sim.disk->expected_finish.store(
                std::chrono::steady_clock::now());
            for (auto &capuch : this->sim.capuches) {
                capuch.thread_state.flush_finish =
                    std::chrono::steady_clock::now();
            }
        } else if (cmd.rfind("conf", 0) == 0) {
            std::string target;
            long value;
            ss >> target >> value;
            auto field = this->sim.conf_map.find(target);
            if (field != this->sim.conf_map.end())
                field->second = value;
        }
        /* Unhandled command */
        else {
            return false;
        }

        return true;
    }

    void update_global_stats(nc_win_txt &txt) {
        std::stringstream ss;
        if (!sim.is_running()) {
            ss << "Simulation not running" << std::endl;
        } else {
            ss << "Simulation is running" << std::endl;
            ss << "Disk write queue(millis)="
               << std::chrono::duration_cast<std::chrono::milliseconds>(
                      sim.disk->expected_finish.load() -
                      std::chrono::steady_clock::now())
                      .count()
               << std::endl;
            ss << "Total pressure=" << sim.p->run.total_pressure << std::endl;
            ss << "Total free=" << sim.p->run.free.size() << std::endl;
            ss << "Locks taken=" << sim.p->stats.locks_taken << std::endl;
            ss << "Buffers lost=" << sim.p->stats.bufs_lost << std::endl;
        }
        txt.lines = ss.str();
    }

    void update_global_conf(nc_win_txt &txt) {
        std::stringstream ss;
        for (auto e : this->sim.conf_map) {
            ss << e.first << ": " << e.second << std::endl;
        }
        txt.lines = ss.str();
    }

    void update_capuch_view(nc_win_txt &txt) {
        std::stringstream ss;
        if (!sim.is_running()) {
            ss << "Simulation not running" << std::endl;
        } else {
            ss << "State" << std::endl;
            ss << std::setw(3) << "N";
            ss << std::setw(9) << "BID";
            ss << std::setw(5) << "frdy";
            ss << std::setw(5) << "f-ng";
            ss << std::setw(4) << "gr";
            ss << std::setw(6) << "prs";
            ss << std::setw(6) << "qta";
            ss << std::setw(6) << "nbf";
            ss << std::setw(5) << "free";
            ss << std::setw(5) << "rdy";
            ss << std::setw(4) << "act";
            ss << std::endl;
            for (auto capuch : this->sim.get_capuches()) {
                ss << std::setw(3) << capuch.id;
                ss << std::setw(9) << std::hex << capuch.batch_id << std::dec;
                ss << std::setw(5) << capuch.thread_state.flush_ready;
                ss << std::setw(5) << capuch.thread_state.flushing;
                ss << std::setw(4) << capuch.greed;
                ss << std::setw(6) << capuch.pressure();
                ss << std::setw(6) << capuch.quota();
                ss << std::setw(6) << capuch.nbufs();
                ss << std::setw(5) << capuch.free_list.size();
                ss << std::setw(5) << capuch.ready_list.size();
                ss << std::setw(4)
                   << (capuch.active_rsc.has_value() ? capuch.active_rsc->id
                                                     : -1);
                ss << std::endl;
            }

            ss << "Settings" << std::endl;
            ss << std::setw(3) << "N";
            ss << std::setw(4) << "run";
            ss << std::setw(5) << "rps";
            ss << std::setw(4) << "pri";
            ss << std::endl;
            for (auto capuch : this->sim.get_capuches()) {
                ss << std::setw(3) << capuch.id;
                ss << std::setw(4) << capuch.simulation.running;
                ss << std::setw(5) << capuch.simulation.ready_per_sec;
                ss << std::setw(4) << capuch.priority;
                ss << std::endl;
            }

            ss << "Stats" << std::endl;
            ss << std::setw(3) << "N";
            ss << std::setw(7) << "greed-";
            ss << std::setw(7) << "greed+";
            ss << std::setw(7) << "t-outs";
            ss << std::endl;
            for (auto capuch : this->sim.get_capuches()) {
                ss << std::setw(3) << capuch.id;
                ss << std::setw(7) << capuch.stats.greed_inc;
                ss << std::setw(7) << capuch.stats.greed_dec;
                ss << std::setw(7) << capuch.stats.timeout;
                ss << std::endl;
            }
        }
        txt.lines = ss.str();
    }

  public:
    static std::string help_string;
    view(simulation &sim) : sim(sim) {}

    void main() {
        ncctx nc;
        nc_lyt_flow flow1(&nc, false);
        nc_lyt_flow flow2(&flow1, true);
        nc_win_txt help(&flow1, "Help");
        help.active = false;
        help.lines = view::help_string;
        nc_lyt_flow flow3(&flow2, false);

        nc_win_inp input(&flow1, "Input commands", ": ");
        input.max_h = 3;

        nc_win_txt capuch_view(&flow2, "Capuch view");
        nc_win_txt global_conf(&flow3, "Global conf");
        nc_win_txt capuch_stats(&flow3, "Global stats");

        input.on_input_listeners.push_back(
            [this, &flow2, &help](nc_win *win,
                                  const std::string &line) -> bool {
                if (line == "help") {
                    help.active = !help.active;
                    flow2.active = !flow2.active;
                }
                return this->command_dispatcher(line);
            });

        nc.set_focus_to(&input);
        while (this->running) {
            this->update_global_stats(capuch_stats);
            this->update_global_conf(global_conf);
            this->update_capuch_view(capuch_view);
            nc.redraw();
            nc.refresh();
            nc.process_input(getch());
        }
    }
};

/* clang-format off */
std::string view::help_string = 
"! Type help again to go back\n"
"\n"
"Navigation:\n"
"  Use TAB to focus window.\n"
"  Use ARROW KEYS to scroll focused window.\n"
"  Type commands inside INPUT box.\n"
"\n"
"Available commands:\n"
"  start => start simulation\n"
"  term => stop simulation\n"
"  quit => close the program\n"
"  capuch START END (speed|priority) VALUE => set capuch speed/priority\n"
"  conf FIELD VALUE => set conf FIELD to VALUE\n"
"    use any conf field fron the configuration window\n"
"    example: pool_conf.min_bufs\n"
"    note: some fields will take effect only after sim stop\n"
"  disk-flush => flush all disk IO immediately\n"
;
/* clang-format on */

int main() {

    {
        simulation sim;
        view view(sim);

        view.main();
    }

    std::cout << "The end" << std::endl;
}
