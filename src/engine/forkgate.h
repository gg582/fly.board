#ifndef FLY_FORKGATE_H
#define FLY_FORKGATE_H

/* Fork gate for threads that use fork-unsafe libraries (sqlite).
 *
 * cwist_app_listen() forks worker children after fly_board has already
 * started its engine pool and cleanup thread. If any thread is inside a
 * sqlite API call at the fork() instant, the child inherits the library's
 * locked static mutexes (they are plain pthread_mutex_t in the library's
 * BSS and survive fork locked) and deadlocks on its first DB access - the
 * atfork child handler reopen_databases_after_fork() stuck in
 * sqlite3_open -> sqlite3_mutex_enter is the observed case, and the
 * parent's shutdown waitpid() then blocks forever.
 *
 * The gate installs a pthread_atfork() prepare handler that waits until
 * every thread inside a marked fork-unsafe section has left it, and keeps
 * new entrants out until the fork completes. Only startup code runs while
 * cwist forks (worker forking happens once, inside cwist_app_listen), so
 * the threads that can collide are the cleanup thread and the GIF warmup
 * pool task; both wrap their sqlite usage with enter()/leave().
 *
 * enter()/leave() are cheap atomics outside the startup fork window, so
 * wrapping daily/background DB work is fine too.
 */
void fly_forkgate_install(void);

/* Mark the current thread as inside a fork-unsafe (sqlite) section.
 * Blocks while a fork is in progress. Must be paired with leave(). */
void fly_forkgate_enter(void);

/* Leave a fork-unsafe section; wakes a waiting fork prepare handler. */
void fly_forkgate_leave(void);

#endif
