/* xboxrecomp-android patch (third_party/parallel-rdp/README.md): a process-wide hook that paraLLEl-RDP's own threads
 * (CommandRing::thread_loop, WorkerThread::main_loop: the command ring worker, the fence executor, the pipeline
 * compile worker) call on themselves as they start (starting = 1) and just before they end (0). The N64 runtime gives
 * it the hook of n64_rdp_set_thread_hook (consoles/n64/runtime/n64_rdp_vk.cpp): on Android the threads join the
 * performance session, run at nice -8 and go back to the fast cores after a resume. MIT, like the rest of this tree. */
#pragma once

namespace RDP
{
using ThreadHook = void (*)(int starting, void *user);

/* For the threads started after the call; nullptr: none (the default). */
void set_thread_hook(ThreadHook fn, void *user);

/* The hook as set when the thread starts, heard again by the same thread as it ends. */
class ThreadHookScope
{
public:
	ThreadHookScope();
	~ThreadHookScope();
	ThreadHookScope(const ThreadHookScope &) = delete;
	ThreadHookScope &operator=(const ThreadHookScope &) = delete;

private:
	ThreadHook fn = nullptr;
	void *user = nullptr;
};
}
